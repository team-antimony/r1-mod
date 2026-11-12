"""Dump the QA weapon-part id -> name table (data\\qa_parts.tsv) from the running QA client.

    uv run --with frida python tools\\data\\dump_qa_parts.py [out.tsv]

Reads the part-name slot table at 0x17E5848 (16384 slots) from the client's own memory; each non-zero slot is a
part id whose name is a C string at the slot pointer (or one indirection on). Output: `id<TAB>name`, id-ordered.
Read-only; the client is not modified.
"""
import sys
from pathlib import Path

import os, sys; sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import fridatmp  # noqa: F401,E402 -- before frida: removes the temp files Frida leaves behind
import frida

DATA = Path(__file__).resolve().parents[2] / 'data'
OUT = Path(sys.argv[1]) if len(sys.argv) > 1 else DATA / 'qa_parts.tsv'

AGENT = r'''
const m = Process.enumerateModules().find(x => x.name.toLowerCase().includes('codomp_client_shipretail'));
const d = m.base.sub(0x400000);
function P(a){ return ptr(a).add(d); }
function rs(p){ try { const s = p.readCString(48); return /^[\x20-\x7e]{2,}/.test(s) ? s.split('�')[0] : null; } catch (e) { return null; } }
rpc.exports = { rows(){
  const out = [], t = P(0x17E5848);
  for (let i = 0; i < 16384; i++) {
    const v = t.add(4 * i).readU32();
    if (!v) continue;
    const p = ptr(v);
    let nm = rs(p);
    if (!nm) { try { nm = rs(p.readPointer()); } catch (e) {} }
    if (nm) out.push([i, nm]);
  }
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
        rows = sc.exports_sync.rows()
    finally:
        s.detach()
    OUT.parent.mkdir(parents=True, exist_ok=True)
    with open(OUT, 'w', encoding='utf-8', newline='\n') as f:
        for i, nm in rows:
            f.write('%d\t%s\n' % (i, nm))
    print('wrote', OUT, len(rows), 'parts')


if __name__ == '__main__':
    main()
