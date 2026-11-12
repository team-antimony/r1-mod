"""Dump the QA client's live weapon table: index -> internal name.

The client keeps WeaponDef pointers in dword_17F5A40[index] (count in dword_7817B88, see sub_42C4D0 /
sub_42C3E0 in qa3333.exe); szInternalName is the first field of a def. This is the index space the client
uses in playerstate/usercmd weapon fields, which need not match the net-const list order.
Usage: uv run --with frida python tools/dump_client_weapons.py [out.tsv]
"""
import sys
from pathlib import Path
import os, sys; sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import fridatmp  # noqa: F401,E402 -- before frida: removes the temp files Frida leaves behind
import frida
DATA = Path(__file__).resolve().parents[2] / 'data'  # repository data folder

out = sys.argv[1] if len(sys.argv) > 1 else str(DATA / 'qa_client_weapons.tsv')
dev = frida.get_local_device()
pid = next(p.pid for p in dev.enumerate_processes() if 'codomp_client_shipretail' in p.name.lower())
s = dev.attach(pid)
sc = s.create_script(r"""
const m = Process.enumerateModules().find(m => m.name.toLowerCase().includes('codomp_client_shipretail'));
const d = m.base.sub(0x400000);
rpc.exports = {
  dump() {
    const count = ptr(0x7817B88).add(d).readU32();
    const rows = [];
    for (let i = 0; i <= count && i < 1024; i++) {
      const def = ptr(0x17F5A40).add(d).add(4 * i).readPointer();
      let name = '';
      try { if (!def.isNull()) name = def.readPointer().readCString(); } catch (e) { name = '?'; }
      rows.push([i, name]);
    }
    return { count, rows };
  }
};
""")
sc.load()
r = sc.exports_sync.dump()
s.detach()
with open(out, 'w', encoding='utf-8') as f:
    for i, name in r['rows']:
        f.write(f'{i}\t{name}\n')
print('count', r['count'], 'written', out)
for i, name in r['rows'][:8]:
    print(i, name)
for i, name in r['rows']:
    if i in (95, 251, 279, 282, 163):
        print('check', i, name)
