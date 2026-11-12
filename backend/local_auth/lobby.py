"""QA protocol 210 lobby handshake."""
from dataclasses import dataclass, field
import hashlib
import hmac
from pathlib import Path
import struct
import time

from .rooms import (CHANNEL_SERVICE, CREATE_CHANNEL_TASK, SEND_MESSAGE_TASK, build_channel_record, schedule_start,
                    start_countdown)
from .storage import PUBLISHER_FILE_TASK, STORAGE_SERVICE, publisher_file_reply

ROOM_CHANNEL_BASE = 69420

# Real publisher files (playlists, configs) go here; missing ones are served empty.
PUBLISHER_ROOT = Path(__file__).resolve().parent.parent / 'publisher'
# Create this file (optionally "ip port") to push a dwRoomMessageStartGame to the client.
STARTGAME_TRIGGER = Path(__file__).resolve().parent.parent / 'startgame.trigger'
STARTGAME_SENDER_ID = 69420
# Same session id as the 1.1.13 lobby, so the sh1x server's default codol_securityId matches.
STARTGAME_SESSION_ID = bytes([70, 0, 0, 0, 0, 0, 0, 0])
SECURITY_KEY_FILE = Path(__file__).resolve().parent.parent / 'state' / 'securitykey.bin'
TICK_SECONDS = 2


def _is_loopback(ip):
    return ip.startswith('127.') or ip in ('localhost', '::1')


def match_session(ip, port, password, event, *, fetch=None):
    """The (security id, security key) a StartGame to ip:port carries, or None when the join cannot go ahead.

    r1-server hands out its own match key (getmatchkey), which is what makes a server on another machine
    joinable. A loopback server that does not answer (an older build) gets today's local key instead: a fresh
    random key written to state/securitykey.bin, which that server reads through codol_securityKeyPath."""
    from .matchkey import MatchKeyRefused, fetch_match_key
    fetch = fetch or fetch_match_key
    try:
        reply = fetch(ip, port, password)
    except MatchKeyRefused as exc:
        event('startgame_refused', host=f'{ip}:{port}', reason=str(exc))
        print(f'r1-mod backend: {ip}:{port} refused the join: {exc}', flush=True)
        return None
    except OSError as exc:
        reply = None
        event('matchkey_unreachable', host=f'{ip}:{port}', reason=str(exc))
    if reply is not None:
        key, session_id = reply
        event('matchkey_received', host=f'{ip}:{port}')
        return session_id, key
    if not _is_loopback(ip):
        event('startgame_no_matchkey', host=f'{ip}:{port}')
        print(f'r1-mod backend: {ip}:{port} did not answer getmatchkey (not an r1-server, or not reachable); '
              'join not started', flush=True)
        return None
    security_key = secrets.token_bytes(16)
    SECURITY_KEY_FILE.parent.mkdir(exist_ok=True)
    SECURITY_KEY_FILE.write_bytes(security_key)
    return STARTGAME_SESSION_ID, security_key


@dataclass(frozen=True)
class Keys:
    c2s_mac: bytes = field(repr=False)
    s2c_mac: bytes = field(repr=False)
    c2s_aes: bytes = field(repr=False)
    s2c_aes: bytes = field(repr=False)


def make_server_header(initial, *, server_id, nonce):
    if len(initial) != 28 or struct.unpack_from('<4I', initial) != (200, 200, 210, 210):
        raise ValueError('Invalid QA initial header')
    if not 26 <= struct.unpack_from('<I', initial, 16)[0] <= 16 * 1024 * 1024:
        raise ValueError('Invalid receive-frame capacity')
    if len(server_id) != 8 or len(nonce) != 8:
        raise ValueError('Server ID and nonce must contain 8 bytes')
    return struct.pack('<I2sI8s8s', 22, b'\xab\x81', 210, server_id, nonce)


def _expand(prk, label, size):
    output = previous = b''
    for counter in range(1, (size + 19) // 20 + 1):
        previous = hmac.digest(prk, previous + label + bytes([counter]), 'sha1')
        output += previous
    return output[:size]


def verify_client_auth(initial, header, packet, lookup, *, now=None, log=None):
    if len(packet) != 153 or packet[:6] != bytes.fromhex('95000000ab82'):
        raise ValueError('Invalid client-auth frame')
    if len(header) != 26 or header != make_server_header(initial, server_id=header[10:18], nonce=header[18:26]):
        raise ValueError('Invalid server transcript header')
    value = int.from_bytes(packet[6:145], 'little')
    if (value & 0xFF != 7 or (value >> 8) & 1 != 1 or
            (value >> 9) & 31 != 8 or (value >> 46) & 31 != 8 or value >> 1107):
        raise ValueError('Malformed client-auth body')
    title = (value >> 14) & 0xFFFFFFFF
    seed = (value >> 51) & 0xFFFFFFFF
    token = ((value >> 83) & ((1 << 1024) - 1)).to_bytes(128, 'little')
    session = lookup(token)
    now = time.time() if now is None else now
    if not session or session['expires'] <= now:
        raise ValueError('Unknown or expired local ticket')
    if title != session['title_id'] or seed != session['iv_seed']:
        raise ValueError('Ticket context mismatch')
    key = session['profile'].auth_key
    if len(key) != 24:
        raise ValueError('Session key must contain 24 bytes')
    transcript_hash = hashlib.sha1(initial[8:] + header + packet[:-8]).digest()
    prk = hmac.digest(transcript_hash, key, 'sha1')
    challenges = _expand(prk, b'CLIENTCHAL', 16)
    data = _expand(prk, b'BDDATA', 72)
    if not hmac.compare_digest(packet[-8:], challenges[:8]):
        raise ValueError('Client proof mismatch')
    reply = struct.pack('<I', 10) + b'\xab\x83' + challenges[8:]
    keys = Keys(data[:20], data[20:40], data[40:56], data[56:72])
    if log is not None:
        log('lobby_keys_derived', c2s_mac=keys.c2s_mac.hex(), s2c_mac=keys.s2c_mac.hex(),
            c2s_aes=keys.c2s_aes.hex(), s2c_aes=keys.s2c_aes.hex())
    return reply, keys, session['profile']


# TCP adapter. Post-auth service dispatch is deliberately not implemented yet.
import secrets
import socket
import socketserver


def _read_exact(conn, length):
    result = bytearray()
    while len(result) < length:
        chunk = conn.recv(length - len(result))
        if not chunk:
            raise EOFError('Peer closed')
        result.extend(chunk)
    return bytes(result)


def _read_frame(conn):
    prefix = _read_exact(conn, 4)
    length = struct.unpack('<I', prefix)[0]
    if length > 524288:
        raise ValueError('Lobby frame exceeds local limit')
    return prefix + _read_exact(conn, length)


class LobbyHandler(socketserver.BaseRequestHandler):
    def handle(self):
        event = self.server.event
        self.request.settimeout(15)
        try:
            initial = _read_exact(self.request, 28)
            header = make_server_header(initial, server_id=secrets.token_bytes(8), nonce=secrets.token_bytes(8))
            self.request.sendall(header)
            event('lobby_header_sent', protocol=210)
            client_auth = _read_frame(self.request)
            def lookup(token):
                with self.server.auth.session_lock:
                    return self.server.auth.sessions.get(token)
            reply, keys, player = verify_client_auth(initial, header, client_auth, lookup, log=event)
            # Log before sending so socket tests and observers see the state transition.
            event('lobby_authenticated', player=player.name, client_auth_size=len(client_auth))
            self.request.sendall(reply)
            expected_counter = 1
            send_counter = 1
            transaction_id = 0
            # Short ticks so a queued start-game push goes out promptly; the keepalive
            # still fires after 20 s of silence and the idle close after 10 minutes.
            self.request.settimeout(TICK_SECONDS)
            idle_checks = 0
            # Room chat channels handed out by 11/13; the last one is where pushes go.
            self.channel_id = ROOM_CHANNEL_BASE
            # monotonic time at which the room's own start countdown ends (None: no start pending)
            self.start_at = None
            while True:
                send_counter = self._send_queued_startgame(keys, send_counter, player, event)
                try:
                    frame = _read_frame(self.request)
                except socket.timeout:
                    idle_checks += 1
                    if idle_checks >= 300:
                        event('lobby_idle_close', player=player.name)
                        break
                    if idle_checks % 10 == 0:
                        self.request.sendall(bytes(4))
                        event('lobby_keepalive', player=player.name)
                    continue
                idle_checks = 0
                if frame == bytes(4):
                    self.request.sendall(bytes(4))
                    continue
                if len(frame) < 50 or frame[4:6] != b'\xab\x85' or (len(frame) - 34) % 16:
                    raise ValueError('Malformed encrypted lobby frame')
                counter = struct.unpack_from('<I', frame, 6)[0]
                if counter != expected_counter:
                    raise ValueError('Lobby counter mismatch')
                from .frames import decode_client_data, encode_server_data, build_task_reply, parse_task
                inner_type, application = decode_client_data(frame, keys, expected_counter=expected_counter)
                expected_counter += 1
                event('lobby_data_verified', player=player.name, counter=counter,
                      frame_bytes=len(frame), dispatch_implemented=True)
                if inner_type != 0x86 or len(application) < 3:
                    raise ValueError('Unsupported client application envelope')
                service, task, args = parse_task(application)
                event('lobby_service_received', player=player.name, service=service, task=task,
                      application_bytes=len(application), args_bytes=len(args),
                      args_hex=args.hex())
                transaction_id += 1
                from .frames import build_register_codoid_result, w_str
                results = ()
                if service == 71 and task == 4:
                    results = (w_str(build_register_codoid_result(player)),)
                elif service == STORAGE_SERVICE and task == PUBLISHER_FILE_TASK:
                    results, note = publisher_file_reply(args, PUBLISHER_ROOT)
                    event('publisher_file_served', player=player.name, **note)
                elif service == CHANNEL_SERVICE and task == CREATE_CHANNEL_TASK:
                    self.channel_id += 1
                    results = (build_channel_record(self.channel_id),)
                    event('room_channel_created', player=player.name, channel_id=self.channel_id)
                elif service == CHANNEL_SERVICE and task == SEND_MESSAGE_TASK and self.server.game_server:
                    # The room host starts the match by broadcasting StartCountingDown (0 s when every
                    # member is Ready or dw_autoStartGame's enter-room countdown ran out); a real
                    # matchmaking service then sends StartGame. Do that once the countdown ends, unless the
                    # host calls it off first (StartCountingDown -1).
                    seconds = start_countdown(args)
                    pending = self.start_at
                    self.start_at = schedule_start(pending, seconds, time.monotonic())
                    if seconds is not None and seconds < 0:
                        event('room_start_cancelled', player=player.name, channel_id=self.channel_id,
                              pending=pending is not None)
                    elif seconds is not None:
                        event('room_start_requested', player=player.name, channel_id=self.channel_id,
                              countdown=seconds, host='%s:%d' % self.server.game_server)
                reply_type, reply_body = build_task_reply(task, transaction_id=transaction_id, results=results)
                event('lobby_reply_built', player=player.name, service=service, task=task,
                      transaction_id=transaction_id, reply_type=reply_type,
                      reply_hex=reply_body.hex())
                response = encode_server_data(reply_type, reply_body, keys, send_counter=send_counter)
                send_counter += 1
                self.request.sendall(response)
                event('lobby_reply_sent', player=player.name, service=service, task=task,
                      transaction_id=transaction_id, frame_bytes=len(response))
        except EOFError:
            event('lobby_peer_closed')
        except (ValueError, OSError) as exc:
            event('lobby_closed', reason=str(exc))

    def _send_queued_startgame(self, keys, send_counter, player, event):
        """Send a StartGame room push when the room's start countdown has ended (to --game-server) or
        STARTGAME_TRIGGER exists ("ip port [password]", default 127.0.0.1 28960; the file is consumed).
        -> next send counter."""
        password = getattr(self.server, 'join_password', '')
        if STARTGAME_TRIGGER.exists():
            target = STARTGAME_TRIGGER.read_text(encoding='utf-8').split(maxsplit=2)
            STARTGAME_TRIGGER.unlink()
            ip, port = (target[0], int(target[1])) if len(target) >= 2 else ('127.0.0.1', 28960)
            if len(target) == 3:
                password = target[2].strip()
        elif self.start_at is not None and time.monotonic() >= self.start_at:
            ip, port = self.server.game_server
        else:
            return send_counter
        self.start_at = None
        from .frames import encode_server_data
        from .rooms import build_room_push, build_startgame_record
        session = match_session(ip, port, password, event)
        if session is None:
            return send_counter
        session_id, security_key = session
        record = build_startgame_record(ip, port, session_id, security_key)
        inner_type, application = build_room_push(record, sender_id=STARTGAME_SENDER_ID,
                                                  sender_name='CODOL', room_id=self.channel_id)
        frame = encode_server_data(inner_type, application, keys, send_counter=send_counter)
        self.request.sendall(frame)
        event('room_startgame_pushed', player=player.name, host=f'{ip}:{port}',
              session_id=session_id.hex(), security_key=security_key.hex(),
              record_hex=record.hex(), frame_bytes=len(frame))
        return send_counter + 1


class LobbyServer(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


def make_lobby_server(auth, *, port=3074, event=None, game_server=('127.0.0.1', 28960), join_password=''):
    """game_server: (ip, port) a room is sent to when its host starts the match; None = only
    startgame.trigger starts one. join_password: sent with getmatchkey when the trigger names none."""
    server = LobbyServer(('127.0.0.1', port), LobbyHandler)
    server.auth = auth
    server.game_server = game_server
    server.join_password = join_password
    server.event = event or (lambda *args, **kwargs: None)
    return server

