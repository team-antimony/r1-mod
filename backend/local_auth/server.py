"""Loopback-only QA HTTPS auth issuer. Lobby protocol is not implemented here."""
import argparse
import base64
import json
import ssl
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from .profiles import ProfileStore
from .tickets import build_auth_response


class AuthHandler(BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'

    def log_message(self, *args):
        pass

    def reply(self, status, result):
        body = json.dumps(result).encode('utf-8')
        self.send_response(status)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(body)))
        self.send_header('Connection', 'close')
        self.end_headers()
        self.wfile.write(body)
        self.close_connection = True

    def do_POST(self):
        if self.path != '/auth/':
            self.server.event('http_request', method='POST', path=self.path)
            return self.reply(404, {'error': 'not_found'})
        try:
            length = int(self.headers.get('Content-Length', '0'))
            if not 0 < length <= 16384:
                raise ValueError('Invalid body size')
            self.connection.settimeout(5)
            body = self.rfile.read(length)
            if len(body) != length:
                raise ValueError('Truncated request')
            request = json.loads(body)
            result = build_auth_response(request, self.server.player)
        except (ValueError, TypeError, OSError) as exc:
            self.server.event('auth_rejected', reason=str(exc))
            return self.reply(400, {'error': 'invalid_auth_request'})
        with self.server.session_lock:
            now = time.time()
            self.server.sessions = {token: value for token, value in self.server.sessions.items()
                                    if value['expires'] > now}
            if len(self.server.sessions) >= 256:
                self.server.sessions.pop(next(iter(self.server.sessions)))
            token = base64.b64decode(result['server_ticket'])
            self.server.sessions[token] = {'profile': self.server.player, 'expires': now + 86400,
                                           'iv_seed': int(request['iv_seed']),
                                           'title_id': int(request['title_id'])}
        self.server.event('auth_issued', player=self.server.player.name,
                          task=result['auth_task'], code=result['code'])
        self.reply(200, result)

    def do_GET(self):
        self.server.event('http_request', method='GET', path=self.path)
        if self.path != '/':
            return self.reply(404, {'error': 'not_found'})
        self.reply(200, {'service': 'codol-qa-local-auth', 'lobby_implemented': False})


def make_server(player, *, port=443, certfile=None, event=None):
    server = ThreadingHTTPServer(('127.0.0.1', port), AuthHandler)
    server.player = player
    server.sessions = {}
    server.session_lock = threading.Lock()
    server.event = event or (lambda *args, **kwargs: None)
    if certfile:
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.minimum_version = ssl.TLSVersion.TLSv1
        context.set_ciphers('ALL:@SECLEVEL=0')
        context.load_cert_chain(certfile)
        server.socket = context.wrap_socket(server.socket, server_side=True)
    return server


def main():
    root = Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--player', required=True)
    parser.add_argument('--port', type=int, default=443)
    parser.add_argument('--lobby-port', type=int, help='Enable the QA lobby handshake on this loopback port')
    parser.add_argument('--stun-port', type=int, default=8000,
                        help='Loopback UDP port for the STUN responder used by the QA '
                             'client address discovery (use 0 to disable)')
    parser.add_argument('--game-server', default='127.0.0.1:28960',
                        help='ip:port a room joins when its host starts the match (Ready, or the enter-room '
                             'countdown); "none" leaves starts to startgame.trigger')
    parser.add_argument('--join-password', default='',
                        help='Password sent to a game server (getmatchkey) when a join names none')
    parser.add_argument('--master-port', type=int, default=20810,
                        help='UDP port of the server list master the r1-mod server browser reads '
                             '(game servers heartbeat to it through sv_master1; use 0 to disable)')
    parser.add_argument('--master-bind', default='127.0.0.1',
                        help='Address the master listens on; 0.0.0.0 lets servers and players on other '
                             'machines use it (a host running a public server list)')
    parser.add_argument('--database', type=Path, default=root / 'data' / 'profiles.sqlite3')
    parser.add_argument('--certificate', type=Path, default=root / 'pki' / 'server.pem')
    args = parser.parse_args()
    player = ProfileStore(args.database).get_or_create(args.player)
    logfile = root / 'logs' / 'local-auth.jsonl'
    logfile.parent.mkdir(exist_ok=True)
    lock = threading.Lock()
    def event(kind, /, **fields):
        row = json.dumps({'time': time.time(), 'event': kind, **fields}, ensure_ascii=True)
        with lock:
            with logfile.open('a', encoding='utf-8') as stream:
                stream.write(row + '\n')
            print(row, flush=True)
    server = make_server(player, port=args.port, certfile=str(args.certificate), event=event)
    lobby = lobby_thread = stun = discovery = master = None
    try:
        if args.stun_port:
            from .stun import make_stun_server
            stun = make_stun_server(port=args.stun_port, log=event)
        if args.master_port:
            from .master import make_master_server
            try:
                master = make_master_server(bind=args.master_bind, port=args.master_port, log=event)
            except OSError as exc:
                # a standalone `python -m local_auth.master` may already hold the port
                event('master_unavailable', port=args.master_port, reason=str(exc))
        if args.lobby_port is not None:
            from .lobby import make_lobby_server
            from .stun import make_stun_server
            # The DW live manager probes the lobby address over UDP (types 30/20/14).
            discovery = make_stun_server(port=args.lobby_port, log=event, name='discovery')
            game_server = None
            if args.game_server.lower() != 'none':
                host, _, game_port = args.game_server.rpartition(':')
                game_server = (host, int(game_port))
            lobby = make_lobby_server(server, port=args.lobby_port, event=event, game_server=game_server,
                                      join_password=args.join_password)
            lobby_thread = threading.Thread(target=lobby.serve_forever, daemon=True)
            lobby_thread.start()
            event('lobby_listening', endpoint=f'127.0.0.1:{args.lobby_port}')
        event('listening', endpoint=f'127.0.0.1:{args.port}', player=player.name)
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
        if stun is not None:
            stun.stop()
        if master is not None:
            master.stop()
        if discovery is not None:
            discovery.stop()
        if lobby is not None:
            lobby.shutdown()
            lobby.server_close()
            if lobby_thread is not None:
                lobby_thread.join(3)


if __name__ == '__main__':
    main()
