"""Dump the QA client's weapon ammo/clip pool indices: the keys the snapshot's clip/reserve lists use.

From the client registration code (qa3333.exe sub_42BFA0 / sub_42C0A0 / sub_42C1B0): a WeaponDef has
szAmmoName +1716 / iAmmoIndex +1720, szClipName +1724 / iClipIndex +1728, shared-cap name +1740 / index +1744,
noClip flag +1712. The index is the weapon's own table index unless a dedupe flag makes it share an earlier
weapon's pool. Output: data/qa_client_weapon_keys.tsv  (index, name, ammoIdx, clipIdx, sharedIdx, ammoName, clipName)
Usage: uv run --with frida python tools/dump_client_weapon_keys.py [out.tsv]
"""
import sys
from pathlib import Path
import os, sys; sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import fridatmp  # noqa: F401,E402 -- before frida: removes the temp files Frida leaves behind
import frida
DATA = Path(__file__).resolve().parents[2] / 'data'  # repository data folder

out = sys.argv[1] if len(sys.argv) > 1 else str(DATA / 'qa_client_weapon_keys.tsv')
dev = frida.get_local_device()
pid = next(p.pid for p in dev.enumerate_processes() if 'codomp_client_shipretail' in p.name.lower())
s = dev.attach(pid)
sc = s.create_script(r"""
const m = Process.enumerateModules().find(m => m.name.toLowerCase().includes('codomp_client_shipretail'));
const d = m.base.sub(0x400000);
function cstr(p) { try { return p.isNull() ? '' : p.readCString(); } catch (e) { return '?'; } }
rpc.exports = {
  dump() {
    const count = ptr(0x7817B88).add(d).readU32();
    const rows = [];
    for (let i = 0; i <= count && i < 1024; i++) {
      const def = ptr(0x17F5A40).add(d).add(4 * i).readPointer();
      if (def.isNull()) { rows.push([i, '', -1, -1, -1, '', '']); continue; }
      rows.push([i, cstr(def.readPointer()),
        def.add(1720).readS32(), def.add(1728).readS32(), def.add(1744).readS32(),
        cstr(def.add(1716).readPointer()), cstr(def.add(1724).readPointer())]);
    }
    return { count, rows };
  }
};
""")
sc.load()
r = sc.exports_sync.dump()
s.detach()
with open(out, 'w', encoding='utf-8') as f:
    for row in r['rows']:
        f.write('\t'.join(str(x) for x in row) + '\n')
print('count', r['count'], 'written', out)
for row in r['rows']:
    if row[0] in (1, 2, 95, 163, 180, 251, 279, 282):
        print(row)
shared = sum(1 for row in r['rows'] if row[0] and row[2] != row[0])
print('weapons sharing an earlier ammo pool:', shared)
