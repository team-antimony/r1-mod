"""Check delta snapshots end to end: python compare_delta.py <server qa_delta_trace.log> [client trace]

Reads r1-server's delta trace (codol_qaDeltaEntities 2: every field it wrote for a changed entity, with the value it
meant) and the client decoder trace from trace_delta.py (the entity the QA client actually rebuilt), matches the
snapshots by server time and compares each written field with the client's memory at that field's offset. It also
checks that the client decoded exactly the entities the server mentioned. Position and angle fields are quantised or
stored obfuscated by the client, so they are counted but not compared.
"""
import re, struct, sys
from pathlib import Path

server = Path(sys.argv[1])
client = Path(sys.argv[2] if len(sys.argv) > 2 else Path(__file__).resolve().parents[2] / 'build' / 'client_delta_trace.log')

# server: last block per server time
blocks, cur = {}, None
pending = []
for line in server.read_text(errors='replace').splitlines():
    m = re.match(r'S (\d+) d(\d+) f(\d+) n(\d+) old(\d+)', line)
    if m:
        cur = {'d': int(m[2]), 'ents': [], 'fields': {}}
        blocks[int(m[1])] = cur
        pending = []
        continue
    if cur is None:
        continue
    m = re.match(r'\s+(\d+) f(\d+) c(-?\d+) @(\d+) ([0-9a-f]{8}) -> ([0-9a-f]{8})', line)
    if m:
        cur['fields'].setdefault(int(m[1]), []).append((int(m[2]), int(m[3]), int(m[4]), int(m[5], 16), int(m[6], 16)))
        continue
    m = re.match(r' ([CRA]) (\d+) t(\d+)', line)
    if m:
        cur['ents'].append((m[1], int(m[2])))

snaps, cur = {}, None
for line in client.read_text(errors='replace').splitlines():
    m = re.match(r'S (\d+) d(\d+) f(\d+)', line)
    if m:
        cur = {'d': int(m[2]), 'ents': [], 'mem': {}, 'over': None}
        snaps[int(m[1])] = cur
        continue
    if cur is None:
        continue
    m = re.match(r' ([DR]) (\d+) from t(-?\d+) to t(-?\d*)', line)
    if m:
        cur['ents'].append((m[1], int(m[2])))
        continue
    m = re.match(r'  X (\d+) ([0-9a-f]+)', line)
    if m:
        cur['mem'][int(m[1])] = bytes.fromhex(m[2])
        continue
    m = re.match(r'E over=(\d+)', line)
    if m:
        cur['over'] = int(m[1])

SKIP = {-83, -82, -81, -79, -100, -87, -64, -63, -62, -61, -60, -59, -86}
both = [t for t in sorted(snaps) if snaps[t]['d'] and t in blocks and blocks[t]['d'] and snaps[t]['over'] is not None]
lists_ok = lists_bad = checked = skipped = wrong = overran = 0
shown = 0
for t in both:
    s, c = blocks[t], snaps[t]
    if c['over']:
        overran += 1
    want = [n for _, n in s['ents']]
    have = [n for _, n in c['ents']]
    if want == have:
        lists_ok += 1
    else:
        lists_bad += 1
        if shown < 5:
            shown += 1
            print(f'time {t}: server mentioned {want}, client decoded {have}')
    for num, fields in s['fields'].items():
        mem = c['mem'].get(num)
        if mem is None:
            continue
        for index, code, offset, old, new in fields:
            if code in SKIP:
                skipped += 1
                continue
            got = struct.unpack_from('<I', mem, offset)[0]
            if 1 <= code <= 31:
                mask = (1 << code) - 1
                ok = (got & mask) == (new & mask) and got >> code == 0
            elif -32 <= code <= -1:
                n = -code
                signed = new & ((1 << n) - 1)
                if signed >> (n - 1):
                    signed |= (0xFFFFFFFF << n) & 0xFFFFFFFF
                ok = got == signed
            elif code == -75:
                ok = got == (new & 0xFF)
            elif code == -76:
                ok = got == (new & 0xFFFFFFFF)
            elif code == -98:
                ok = got == (new & 0x3FFFFFFF)
            elif code == -93:
                ok = (got & 2047) == (new & 2047)
            else:                                   # 32, 0, -88, -89, -99, -94 type, -96, times, raw longs
                ok = got == new
            checked += 1
            if not ok:
                wrong += 1
                if shown < 25:
                    shown += 1
                    print(f'time {t} entity {num} field {index} code {code} @{offset}: server {old:08x} -> {new:08x}, client has {got:08x}')
print(f'{len(both)} delta snapshots in both traces: entity lists equal in {lists_ok}, different in {lists_bad}, overran {overran}')
print(f'{checked} field values compared, {wrong} wrong; {skipped} position/angle fields not compared')
