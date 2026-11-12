"""Server list master for the r1-mod server browser (Quake 3 / CoD4 master protocol subset).

Dedicated servers send `heartbeat <game>` out of band to sv_master1 every minute (`heartbeat
flatline` when they shut down); the master keeps each sender's address for HEARTBEAT_TTL seconds.
`getservers ...` is answered with one or more `getserversResponse` datagrams that list the live
servers as `\\` + 4-byte IPv4 + 2-byte port (network order) and end with `\\EOT\\0\\0\\0`. The browser
then asks each server for its map and players with `getinfo` itself.

Runs inside `local_auth.server` (--master-port, default 20810) or on its own:
`python -m local_auth.master [--port 20810] [--bind 127.0.0.1]`.
"""
import argparse
import json
import socket
import threading
import time

OOB = b'\xff\xff\xff\xff'
HEARTBEAT_TTL = 200.0           # three missed one-minute heartbeats
SERVERS_PER_PACKET = 200        # 7 bytes each, well under one datagram
FLATLINE = 'flatline'


class ServerList:
    """Live servers keyed by (ip, port); thread safe."""

    def __init__(self, ttl=HEARTBEAT_TTL, clock=time.monotonic):
        self._ttl = ttl
        self._clock = clock
        self._lock = threading.Lock()
        self._servers = {}

    def heartbeat(self, address, game):
        """-> 'added', 'refreshed' or 'removed' (for `flatline`)."""
        with self._lock:
            if game == FLATLINE:
                return 'removed' if self._servers.pop(address, None) is not None else 'unknown'
            known = address in self._servers
            self._servers[address] = (self._clock(), game)
            return 'refreshed' if known else 'added'

    def live(self):
        now = self._clock()
        with self._lock:
            for address in [a for a, (seen, _) in self._servers.items() if now - seen > self._ttl]:
                del self._servers[address]
            return sorted(self._servers)


def parse_request(data):
    """-> (command, arguments) of an out-of-band datagram, or None."""
    if not data.startswith(OOB):
        return None
    words = data[len(OOB):].split(b'\n', 1)[0].decode('latin-1').split()
    return (words[0].lower(), words[1:]) if words else None


def build_getservers_responses(servers):
    """List of datagrams for `servers` [(ip, port)]; at least one, the last ends with EOT."""
    packets = []
    for start in range(0, max(len(servers), 1), SERVERS_PER_PACKET):
        body = bytearray(OOB + b'getserversResponse\n')
        for ip, port in servers[start:start + SERVERS_PER_PACKET]:
            body += b'\\' + socket.inet_aton(ip) + port.to_bytes(2, 'big')
        packets.append(body)
    packets[-1] += b'\\EOT\x00\x00\x00'
    return [bytes(p) for p in packets]


class MasterServer(threading.Thread):

    def __init__(self, bind='127.0.0.1', port=20810, log=None, servers=None):
        super().__init__(name='master-server', daemon=True)
        self.log = log or (lambda *a, **k: None)
        self.servers = servers or ServerList()
        self._socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self._socket.bind((bind, port))
        self.bind_address = self._socket.getsockname()
        self._stop = threading.Event()

    def handle(self, data, source):
        """-> datagrams to send back to `source`."""
        request = parse_request(data)
        if request is None:
            return []
        command, args = request
        if command == 'heartbeat':
            game = args[0] if args else ''
            state = self.servers.heartbeat(source, game)
            self.log('master_heartbeat', server=f'{source[0]}:{source[1]}', game=game, state=state)
            return []
        if command == 'getservers':
            live = self.servers.live()
            self.log('master_getservers', client=f'{source[0]}:{source[1]}', servers=len(live))
            return build_getservers_responses(live)
        return []

    def run(self):
        self.log('master_listening', endpoint=f'{self.bind_address[0]}:{self.bind_address[1]}')
        while not self._stop.is_set():
            try:
                data, source = self._socket.recvfrom(2048)
            except ConnectionResetError:
                # Windows reports an ICMP port-unreachable for an earlier reply here.
                continue
            except OSError:
                break
            for packet in self.handle(data, source):
                try:
                    self._socket.sendto(packet, source)
                except OSError as exc:
                    self.log('master_send_error', client=f'{source[0]}:{source[1]}', reason=str(exc))

    def stop(self):
        self._stop.set()
        try:
            self._socket.close()
        except OSError:
            pass


def make_master_server(bind='127.0.0.1', port=20810, log=None):
    server = MasterServer(bind=bind, port=port, log=log)
    server.start()
    return server


def main():
    parser = argparse.ArgumentParser(description='Server list master for the r1-mod server browser')
    parser.add_argument('--bind', default='127.0.0.1')
    parser.add_argument('--port', type=int, default=20810)
    args = parser.parse_args()

    def event(kind, /, **fields):
        print(json.dumps({'time': time.time(), 'event': kind, **fields}, ensure_ascii=True), flush=True)

    server = make_master_server(bind=args.bind, port=args.port, log=event)
    try:
        while server.is_alive():
            server.join(1)
    except KeyboardInterrupt:
        pass
    finally:
        server.stop()


if __name__ == '__main__':
    main()
