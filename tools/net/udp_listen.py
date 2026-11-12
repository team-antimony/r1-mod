"""Log every UDP datagram the client sends: python udp_listen.py [port] [logfile]"""
import socket, sys, time
port = int(sys.argv[1]) if len(sys.argv) > 1 else 28969
log = open(sys.argv[2] if len(sys.argv) > 2 else 'udp_listen.log', 'a', buffering=1, encoding='utf8')
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.bind(('127.0.0.1', port))
log.write(f'listening on 127.0.0.1:{port}\n')
while True:
    data, addr = s.recvfrom(4096)
    log.write(f'{time.strftime("%H:%M:%S")} {addr} {len(data)}B {data[:96]!r}\n')
    if len(sys.argv) > 3 and sys.argv[3] == '--reply':
        s.sendto(b'\xff\xff\xff\xffprint\nhello from udp_listen\n', addr)

