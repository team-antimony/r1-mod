"""Decode native-server snapshots captured by tshark (srcport<TAB>hex) with the research decoder.

uv run --with cryptography python tools/net/native_snap_decode.py build/native_snap.txt [count]
The netchan body is u32 seq, u32 reliableAck, XOR payload; the XOR key byte is found by
requiring the payload to start with svc 0 (snapshot).
"""
import importlib.util
import json
import struct
import sys
from pathlib import Path
from types import SimpleNamespace

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / 'backend'))
sys.path.insert(0, str(Path(__file__).resolve().parent))
from local_auth.dtls import derive_key, _crypt  # noqa: E402
from codolnet.cipher import xor_payload  # noqa: E402
from codolnet.msg import MsgReader  # noqa: E402

ANALYSIS = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('oracle', ANALYSIS / 'qa_snapshot_oracle.py')
oracle = importlib.util.module_from_spec(spec)
spec.loader.exec_module(oracle)
rows = json.loads((ANALYSIS / 'qa_netfields_aligned.json').read_text(encoding='utf-8'))
ps_table = [tuple(r[2:5]) for r in rows if r[0] == 'playerState']
clients = [tuple(r) for r in json.loads(Path(__file__).resolve().parents[2] / 'data' / 'server' / 'qa_client_fields.json'.read_text(encoding='utf-8'))]


class OracleReader(MsgReader):
    readbit = MsgReader.read_bit
    bits = MsgReader.read_bits
    byte = MsgReader.read_u8
    short = MsgReader.read_i16
    long = MsgReader.read_u32


STATE = REPO / 'backend' / 'state'  # the server home run_native.ps1 uses
line = open(STATE / 'sh1x_home' / 'codol_sessions.txt').read().split('\n')[0].split()
key = derive_key(bytes.fromhex(line[2]), open(STATE / 'securitykey.bin', 'rb').read())
limit = int(sys.argv[2]) if len(sys.argv) > 2 else 3
done = 0
for l in open(sys.argv[1]):
    parts = l.split()
    if len(parts) != 2 or parts[0] != '28960':
        continue
    r = bytes.fromhex(parts[1])
    if r[:2] != b'\x06\x02':
        continue
    tag, low = struct.unpack_from('<HH', r, 2)
    body = _crypt(r[16:], key, low, tag)
    seq, ack = struct.unpack_from('<II', body)
    if seq & 0x80000000:
        print('fragment', hex(seq))
        continue
    payload = body[8:]
    # empty command text: the key byte stays constant, so it is payload[0] ^ svc 0
    plain = xor_payload(payload, payload[0], b'')
    print(f'seq={seq} ack={ack} len={len(plain)} hex={plain.hex()}')
    profile = SimpleNamespace(ps_fields=ps_table, ps_tail='qa', ammo_bits=(10, 9), clip_bits=(10, 11),
                              anim_format='qa', new24=(), clients=clients)
    dec = oracle.Snapshots(profile)
    rd = OracleReader(plain)
    try:
        while True:
            op = rd.byte()
            if op == 0:
                info = dec.snapshot(rd, seq)
                print('  snapshot', info)
                print('  ps', dec.frames[seq].get('ps'))
            elif op == 6:
                print('  EOF, remaining', rd.remaining())
                break
            else:
                print('  op', op, 'unhandled; stop')
                break
    except Exception as e:  # noqa: BLE001
        print('  decode error', type(e).__name__, e)
    done += 1
    if done >= limit:
        break
