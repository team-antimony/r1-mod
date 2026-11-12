"""Dump the QA player-animation index -> name table (data\\qa_player_anims.tsv) from the running QA client.

    uv run --with frida python tools\\data\\dump_qa_player_anims.py [out.tsv]

Reads the animation-name array through the pointer at 0x7817B84 (104-byte records, count at +106496) from the
client's own memory. Each name is read up to the first non-printable byte, which trims the record padding.
Output: `index<TAB>name`. Read-only; the client is not modified.
"""
import sys
from pathlib import Path

import os, sys; sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import fridatmp  # noqa: F401,E402 -- before frida: removes the temp files Frida leaves behind
import frida

DATA = Path(__file__).resolve().parents[2] / 'data'
OUT = Path(sys.argv[1]) if len(sys.argv) > 1 else DATA / 'qa_player_anims.tsv'

AGENT = r'''
const m = Process.enumerateModules().find(x => x.name.toLowerCase().includes('codomp_client_shipretail'));
const d = m.base.sub(0x400000);
function P(a){ return ptr(a).add(d); }
function name(p){ let s = ''; for (let k = 0; k < 63; k++) { const c = p.add(k).readU8(); if (c < 0x20 || c > 0x7e) break; s += String.fromCharCode(c); } return s; }
rpc.exports = { rows(){
  const b4 = P(0x7817B84).readPointer();
  if (b4.isNull()) return [];
  const n = b4.add(106496).readU32(), out = [];
  for (let i = 0; i < n; i++) out.push([i, name(b4.add(104 * i))]);
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
    if not rows:
        raise SystemExit('player-anim table not loaded (pointer at 0x7817B84 is null) - join a match and retry')
    OUT.parent.mkdir(parents=True, exist_ok=True)
    with open(OUT, 'w', encoding='utf-8', newline='\n') as f:
        for i, nm in rows:
            f.write('%d\t%s\n' % (i, nm))
    print('wrote', OUT, len(rows), 'anim names')


if __name__ == '__main__':
    main()
