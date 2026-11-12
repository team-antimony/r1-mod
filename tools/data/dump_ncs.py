"""Dump the QA client's net-const configstrings (index, category, string) from a running client.

The client (sub_584ED0, called from sub_5850B0) walks per-category lists of NetConstStrings assets at
0x2DF7CE8 (pairs: list head, running count) and assigns each asset's strings consecutive configstring
indices from base + offset of the category's row in the layout table at 0xC57118 (base, count, offset).
Read-only frida attach. Output: data/qa_ncs.tsv  (index<TAB>category<TAB>string)
"""
import sys
from pathlib import Path

import os, sys; sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import fridatmp  # noqa: F401,E402 -- before frida: removes the temp files Frida leaves behind
import frida
DATA = Path(__file__).resolve().parents[2] / 'data'  # repository data folder

CATS = ['mdl', 'mat', 'veh', 'vfx', 'loc', 'snd', 'snl', 'shk', 'mnu', 'tag', 'hic', 'mic', 'sel', 'wep',
        'hnt', 'fxt', 'ntk', 'sts']
dev = frida.get_local_device()
pid = next(p.pid for p in dev.enumerate_processes() if 'codomp_client_shipretail' in p.name.lower())
s = dev.attach(pid)
sc = s.create_script(r"""
const m = Process.enumerateModules().find(m => m.name.toLowerCase().includes('codomp_client_shipretail'));
const d = m.base.sub(0x400000);
const A = va => ptr(va).add(d);
rpc.exports = {
  dump() {
    const out = [];
    for (let t = 0; t < 18; t++) {
      const base = A(0xC57118 + 12 * t).readU32(), off = A(0xC57120 + 12 * t).readU32();
      let node = A(0x2DF7CE8 + 8 * t).readPointer();
      let running = 0, guard = 0;
      while (!node.isNull() && guard++ < 256) {
        const asset = node.readPointer();
        const count = asset.add(12).readU32();
        const strs = asset.add(16).readPointer();
        const name = asset.readPointer().readCString();
        for (let i = 0; i < count; i++) {
          let text = '';
          try { text = strs.add(4 * i).readPointer().readCString(); } catch (e) { text = '?'; }
          out.push([base === 7058 ? -1 : base + off + running + i, t, text, name]);
        }
        running += count;
        node = node.add(4).readPointer();
      }
    }
    return out;
  }
};
""")
sc.load()
rows = sc.exports_sync.dump()
s.detach()
out = Path(sys.argv[1] if len(sys.argv) > 1 else str(DATA / 'qa_ncs.tsv'))
with out.open('w', encoding='utf-8') as f:
    for idx, t, text, asset in rows:
        f.write(f'{idx}\t{CATS[t]}\t{text}\t{asset}\n')
from collections import Counter
print(len(rows), 'strings;', dict(Counter(CATS[t] for _, t, _, _ in rows)))
print('assets:', sorted({a for *_, a in rows}))
