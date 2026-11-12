"""Decode native-server QA snapshot dumps (codol_qaDumpSnapshots: hex lines, u32 reliableAck + svc stream,
pre-netchan so no XOR/DTLS) with the research decoder.

uv run python tools/net/decode_dump.py build/qa_snapdump.txt [count]
"""
import importlib.util
import json
import struct
import sys
from pathlib import Path
from types import SimpleNamespace

sys.path.insert(0, str(Path(__file__).resolve().parent))
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


limit = int(sys.argv[2]) if len(sys.argv) > 2 else 2
for n, line in enumerate(open(sys.argv[1])):
    if n >= limit:
        break
    body = bytes.fromhex(line.strip())
    ack = struct.unpack_from('<I', body)[0]
    plain = body[4:]
    print(f'--- message {n}: ack={ack} len={len(plain)} head={plain[:24].hex()}')
    profile = SimpleNamespace(ps_fields=ps_table, ps_tail='qa', ammo_bits=(10, 9), clip_bits=(10, 11),
                              anim_format='qa', new24=(), clients=clients)
    dec = oracle.Snapshots(profile)
    dec.debug = True
    rd = OracleReader(plain)
    try:
        while True:
            op = rd.byte()
            if op == 0:
                info = dec.snapshot(rd, n + 1)
                print('  snapshot', info)
                ps = dec.frames[n + 1].get('ps', {})
                print('  ps fields', len(ps), {k: ps[k] for k in list(ps)[:12]})
                print('  weapon fields', {n: ps.get(o) for o, n in ((620, 'weapState0.weapon'), (636, 'weaponstate'), (832, 'weapCommon.weapon'), (836, 'offhand'), (4, 'pm_type'), (12, 'pm_flags'))})
            elif op == 1:
                seq = rd.read_u32()
                text = rd.read_string(1024)
                print('  servercommand', seq, text[:80])
            elif op == 2:
                seq = rd.read_u32()
                ln = rd.read_i16()
                rd.read_raw(ln)
                print('  binarycommand', seq, ln)
            elif op == 6:
                print('  EOF, remaining', rd.remaining(), 'overflow', rd.overflow)
                break
            else:
                print('  op', op, 'unhandled; stop')
                break
    except Exception as e:  # noqa: BLE001
        print('  decode error', type(e).__name__, e)
