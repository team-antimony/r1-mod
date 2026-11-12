"""Dump the QA camo-item table (data\\qa_weapon_camos.tsv) from the running QA client.

    uv run --with frida python tools\\data\\dump_qa_weapon_camos.py [out.tsv]

Reads the StringTable asset `mp/coweapitems/cam.csv` (type 37) from the client: `item<TAB>weapon<TAB>camo index`
(the camo index is field 3 of the composed weapon name). A direct dump of the three columns. Read-only.
"""
import sys
from pathlib import Path

import os, sys; sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import fridatmp  # noqa: F401,E402 -- before frida: removes the temp files Frida leaves behind
import frida

DATA = Path(__file__).resolve().parents[2] / 'data'
OUT = Path(sys.argv[1]) if len(sys.argv) > 1 else DATA / 'qa_weapon_camos.tsv'
ASSET = 'mp/coweapitems/cam.csv'
HEADER = '# QA 3.3.3.3 camo items (client %s, read live): item<TAB>weapon<TAB>camo index (composed name field 3)' % ASSET

AGENT = r'''
const m = Process.enumerateModules().find(x => x.name.toLowerCase().includes('codomp_client_shipretail'));
const d = m.base.sub(0x400000);
const Req = new NativeFunction(ptr(0x49E7D0).add(d), 'pointer', ['int', 'pointer', 'int'], 'mscdecl');
rpc.exports = { table(name){
  const a = Req(37, Memory.allocUtf8String(name), 1);
  if (a.isNull()) return null;
  const cols = a.add(4).readS32(), rows = a.add(8).readS32(), vals = a.add(12).readPointer(), out = [];
  for (let r = 0; r < rows; r++) { const row = []; for (let c = 0; c < cols; c++) { const p = vals.add(8 * (r * cols + c)).readPointer(); row.push(p.isNull() ? '' : (p.readCString() || '')); } out.push(row); }
  return out;
}};
'''


def main():
    dev = frida.get_local_device()
    pid = max(p.pid for p in dev.enumerate_processes() if 'codomp_client_shipretail' in p.name.lower())
    s = dev.attach(pid)
    try:
        sc = s.create_script(AGENT)
        sc.load()
        rows = sc.exports_sync.table(ASSET)
    finally:
        s.detach()
    if not rows:
        raise SystemExit('%s not loaded in the client' % ASSET)
    OUT.parent.mkdir(parents=True, exist_ok=True)
    with open(OUT, 'w', encoding='utf-8', newline='\n') as f:
        f.write(HEADER + '\n')
        for row in rows:
            f.write('\t'.join(row[:3]) + '\n')
    print('wrote', OUT, len(rows), 'camo items')


if __name__ == '__main__':
    main()
