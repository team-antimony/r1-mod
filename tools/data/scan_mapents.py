"""Find the loaded map's entity string in the QA client and print spawn origins (read-only frida scan)."""
import re
import os, sys; sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import fridatmp  # noqa: F401,E402 -- before frida: removes the temp files Frida leaves behind
import frida

dev = frida.get_local_device()
pid = next(p.pid for p in dev.enumerate_processes() if 'codomp_client_shipretail' in p.name.lower())
s = dev.attach(pid)
sc = s.create_script(r"""
rpc.exports = {
  scan() {
    const hits = [];
    const pat = '6d 70 5f 64 6d 5f 73 70 61 77 6e';   // mp_dm_spawn
    for (const r of Process.enumerateRanges('r--')) {
      if (r.size > 256 * 1024 * 1024) continue;
      try {
        for (const m of Memory.scanSync(r.base, r.size, pat)) {
          let start = m.address.sub(200);
          if (start.compare(r.base) < 0) start = r.base;
          hits.push(Array.from(new Uint8Array(start.readByteArray(600))));
          if (hits.length > 400) return hits;
        }
      } catch (e) {}
    }
    return hits;
  }
};
""")
sc.load()
blocks = sc.exports_sync.scan()
s.detach()
print('hits', len(blocks))
if blocks:
    print(repr(bytes(blocks[0]).decode('latin-1'))[:400])
seen = {}
for b in blocks:
    text = bytes(b).decode('latin-1')
    for m in re.finditer(r'"origin" "([-\d. ]+)"[^{}]*?"classname" "(mp_[a-z_]+)"|"classname" "(mp_[a-z_]+)"[^{}]*?"origin" "([-\d. ]+)"', text):
        cls = m.group(2) or m.group(3)
        org = m.group(1) or m.group(4)
        seen.setdefault(cls, set()).add(org)
for cls, orgs in sorted(seen.items()):
    print(cls, len(orgs), sorted(orgs)[:6])
