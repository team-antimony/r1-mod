"""Minimal handshake responder that logs the client's `stats` packets: python oob_probe.py [seconds]"""
import random, re, socket, struct, sys, time
secs = float(sys.argv[1]) if len(sys.argv) > 1 else 15
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.bind(('127.0.0.1', 28969)); s.settimeout(0.5)
OOB = b'\xff\xff\xff\xff'
chal = {}
seen = {}
end = time.time() + secs
while time.time() < end:
    try:
        d, a = s.recvfrom(4096)
    except socket.timeout:
        continue
    if d[:4] != OOB:
        print('inband', len(d)); continue
    p = d[4:]
    if p.startswith(b'getchallenge'):
        chal[a] = random.randrange(1, 0x7fffffff)
        s.sendto(OOB + b'challengeResponse %d' % chal[a], a)
    elif p.startswith(b'connect'):
        s.sendto(OOB + b'connectResponse \\mapname\\mp_killhouse_sh\\gametype\\war', a)
        print('connect', p[:80])
    elif p.startswith(b'stats'):
        idx = p[6]; n = struct.unpack('<H', p[7:9])[0]
        seen.setdefault(idx, (n, len(p) - 9))
        s.sendto(OOB + b'statResponse 0', a)
    else:
        print('oob', p[:60])
print('stats packets seen (index -> (u16, datalen)):', sorted(seen.items()))
