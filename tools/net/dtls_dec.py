"""Decrypt tshark 'srcport<TAB>hex' lines of QA bdDTLS records using the native server's saved association."""
import struct
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / 'backend'))
from local_auth.dtls import derive_key, record_mac, _crypt  # noqa: E402

STATE = REPO / 'backend' / 'state'  # the server home run_native.ps1 uses
line = open(STATE / 'sh1x_home' / 'codol_sessions.txt').read().split('\n')[0].split()
key = derive_key(bytes.fromhex(line[2]), open(STATE / 'securitykey.bin', 'rb').read())
limit = int(sys.argv[2]) if len(sys.argv) > 2 else 6
want = sys.argv[3] if len(sys.argv) > 3 else None
n = 0
for l in open(sys.argv[1]):
    parts = l.split()
    if len(parts) != 2:
        continue
    port, hx = parts
    if want and port != want:
        continue
    r = bytes.fromhex(hx)
    if r[:2] != b'\x06\x02':
        print(port, 'non-data', r[:8].hex())
        continue
    tag, low = struct.unpack_from('<HH', r, 2)
    ok = record_mac(key, r[:6], r[14:16], r[16:]) == r[6:14]
    # counters this early are below 0x10000, so the low 16 bits are the full counter
    p = _crypt(r[16:], key, low, tag)
    print(port, low, 'mac-ok' if ok else 'MAC-BAD', len(p), p.hex())
    n += 1
    if n >= limit:
        break
