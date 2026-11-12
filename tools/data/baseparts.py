"""Read-only: per QA weapon id 1..288, the allowed part ids (word_17F8248[128*id] pairs part,model;
count byte_17F5848[id]) -> data/qa_base_parts.tsv (id<TAB>part,part,...)."""
import os, sys; sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import fridatmp  # noqa: F401,E402 -- before frida: removes the temp files Frida leaves behind
import frida
from pathlib import Path
DATA = Path(__file__).resolve().parents[2] / 'data'  # repository data folder
dev = frida.get_local_device()
pid = max(p.pid for p in dev.enumerate_processes() if 'codomp_client_shipretail' in p.name.lower())
s = dev.attach(pid)
try:
    sc = s.create_script(r"""
const m=Process.enumerateModules().find(m=>m.name.toLowerCase().includes('codomp_client_shipretail'));
const d=m.base.sub(0x400000); function P(a){return ptr(a).add(d);}
rpc.exports={ r(){ const out=[]; for(let id=1; id<289; id++){ const n=P(0x17F5848).add(id).readU8(); const row=P(0x17F8248).add(256*id); const parts=[];
  for(let k=0;k<n && k<64;k++) parts.push(row.add(4*k).readU16()); out.push([id,parts]); } return out; }};
""")
    sc.load(); r = sc.exports_sync.r()
finally:
    s.detach()
with open(str(DATA / 'qa_base_parts.tsv'), 'w', newline='\n') as f:
    for i, parts in r:
        if parts: f.write(f"{i}\t{','.join(map(str, parts))}\n")
print('bases with parts', sum(1 for i, p in r if p), 'sample', r[4][:1], r[4][1][:12])
