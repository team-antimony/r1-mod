"""Read rank/experience/prestige from the QA client's live secure playerdata buffer.

Secure buffer: (&0xC95BD80 + 37304); byte offsets come from the dumped
mp/playersecuredata.def (schema offsets[logical]: experience 4840, prestige 4879, rank 4883, rankVersion 4887;
int members are read as 32-bit at that byte offset, bools as bit indices, shorts as byte offsets).
rank is 1-based (rankTable row + 1) and experience is XP within that rank, so level 70 is
rank=70,experience=0,rankVersion=4 (rankVersion = maxrank row column 3). r1-mod `cx_stats` keeps these applied.
Usage: uv run --with frida python tools/read_securedata.py [--set rank=70,experience=0,rankVersion=4]
"""
import sys
import os, sys; sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import fridatmp  # noqa: F401,E402 -- before frida: removes the temp files Frida leaves behind
import frida

SECURE = 0xC95BD80 + 0x91B8   # 37304 bytes; CL_BinCmd_PlayerStats: base + 9326 dwords
PUBLIC = 0xC95BD80 + 144
FIELDS = {'experience': 4840, 'prestige': 4879, 'rank': 4883, 'rankVersion': 4887, 'elo': 4832, 'skill': None}
sets = {}
if len(sys.argv) > 2 and sys.argv[1] == '--set':
    for kv in sys.argv[2].split(','):
        k, v = kv.split('=')
        sets[k] = int(v)

dev = frida.get_local_device()
pid = next(p.pid for p in dev.enumerate_processes() if 'codomp_client_shipretail' in p.name.lower())
s = dev.attach(pid)
sc = s.create_script(r"""
const m = Process.enumerateModules().find(m => m.name.toLowerCase().includes('codomp_client_shipretail'));
const d = m.base.sub(0x400000);
rpc.exports = {
  read(base, offs) { const b = ptr(base).add(d); const o = {}; for (const k in offs) if (offs[k] !== null) o[k] = b.add(offs[k]).readU32(); o.head = Array.from(new Uint8Array(b.readByteArray(16))); o.pub_head = Array.from(new Uint8Array(ptr(base - 37304 + 144).add(d).readByteArray(16))); o.pub_class0 = ptr(base - 37304 + 144 + 8689).add(d).readCString(); return o; },
  write(base, off, value) { ptr(base).add(d).add(off).writeU32(value); return ptr(base).add(d).add(off).readU32(); }
};
""")
sc.load()
r = sc.exports_sync.read(SECURE, FIELDS)
print('secure @ +37304:', {k: v for k, v in r.items() if k not in ('head','pub_head','pub_class0')}, 'head', bytes(r['head']).hex())
print('public control: head', bytes(r['pub_head']).hex(), 'class0 name', r['pub_class0'])
for k, v in sets.items():
    print('set', k, '->', sc.exports_sync.write(SECURE, FIELDS[k], v))
s.detach()
