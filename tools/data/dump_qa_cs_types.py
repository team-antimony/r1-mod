"""Dump the QA configstring type map (data\\qa_cs_types.bin) from the running QA client.

    uv run --with frida python tools\\data\\dump_qa_cs_types.py [out.bin]

r1-server needs this file (codol_qaTypesPath) to build a gamestate the QA client accepts. The client fills the
7058-byte map at 0x2DF7DA0 when it initializes its net-const namespace (flag 0x2DF9934 == 1, set once the client
has loaded a match's constants: join a match first). The map is written byte for byte, and the client's own
configstring checksum (0x2DF7D90) is printed for reference. Read-only; the client is not modified.
"""
import sys
from pathlib import Path

import os, sys; sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import fridatmp  # noqa: F401,E402 -- before frida: removes the temp files Frida leaves behind
import frida

DATA = Path(__file__).resolve().parents[2] / 'data'
OUT = Path(sys.argv[1]) if len(sys.argv) > 1 else DATA / 'qa_cs_types.bin'
SIZE = 7058

AGENT = r'''
const m = Process.enumerateModules().find(x => x.name.toLowerCase().includes('codomp_client_shipretail'));
const d = m.base.sub(0x400000);
function P(a){ return ptr(a).add(d); }
rpc.exports = { read(){
  return { ready: P(0x2DF9934).readU32(), checksum: P(0x2DF7D90).readU32(),
           types: Array.from(new Uint8Array(P(0x2DF7DA0).readByteArray(%d))) };
}};
''' % SIZE


def main():
    dev = frida.get_local_device()
    pid = max(p.pid for p in dev.enumerate_processes() if 'codomp_client_shipretail' in p.name.lower())
    s = dev.attach(pid)
    try:
        sc = s.create_script(AGENT)
        sc.load()
        got = sc.exports_sync.read()
    finally:
        s.detach()
    if got['ready'] != 1:
        raise SystemExit('the client has not initialized its configstring types yet: join a match, then run this again')
    data = bytes(got['types'])
    if len(data) != SIZE:
        raise SystemExit('read %d bytes, expected %d' % (len(data), SIZE))
    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_bytes(data)
    print('wrote', OUT, len(data), 'bytes; client configstring checksum', got['checksum'])


if __name__ == '__main__':
    main()
