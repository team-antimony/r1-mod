# QA replicated dvars (sub_579250): names CS 24+i, values CS 344+i; shows each value's source and the dvar's live value.
#   uv run --with frida python tools\data\repldvars.py            print every slot
#   uv run --with frida python tools\data\repldvars.py --tsv [out] write data\qa_replicated_dvars.tsv for r1-server
#                                                                  (slot, name, client net-const default; join a match first)
import sys
from pathlib import Path
import os, sys; sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import fridatmp  # noqa: F401,E402 -- before frida: removes the temp files Frida leaves behind
import frida, json
TSV = '--tsv' in sys.argv
OUT = Path(next((a for a in sys.argv[1:] if a != '--tsv'), Path(__file__).resolve().parents[2] / 'data' / 'qa_replicated_dvars.tsv'))
dev = frida.get_local_device()
pid = max(p.pid for p in dev.enumerate_processes() if 'codomp_client_shipretail' in p.name.lower())
s = dev.attach(pid)
try:
    sc = s.create_script(r"""
const m=Process.enumerateModules().find(m=>m.name.toLowerCase().includes('codomp_client_shipretail'));
const d=m.base.sub(0x400000); function P(a){return ptr(a).add(d);}
function raw(i){ return P(0x7976124).add(P(0x796F2DC).add(4*i).readU32()); }
function ncs(i){ const t=P(0x2DF7DA0).add(i).readU8(); let v=P(0x2DF7CE8).add(8*t).readPointer();
  const k=i-P(0xC57120).add(12*t).readU32()-P(0xC57118).add(12*t).readU32(); let b=0;
  while(!v.isNull()){ const a=v.readPointer(); const c=a.add(12).readU32(); if(b+c>k) return a.add(16).readPointer().add(4*(k-b)).readPointer().readCString(); v=v.add(4).readPointer(); b+=c; }
  return null; }
function cs(i){ const t=P(0x2DF7DA0).add(i).readU8(); if(t>=0x15) return raw(i).readCString();
  if(t==19){ const r=raw(i).readCString(); if(r) return r; } return ncs(i); }
rpc.exports={ r(){ const out=[]; for(let i=0;i<320;i++){ const n=cs(24+i); if(!n) break;
   out.push({i, name:n, value:cs(344+i), server:raw(344+i).readCString(), ncs:ncs(344+i)}); } return out; }};
""")
    sc.load()
    rows = sc.exports_sync.r()
finally:
    s.detach()
if not TSV:
    for e in rows: print(e)
else:
    OUT.parent.mkdir(parents=True, exist_ok=True)
    with open(OUT, 'w', encoding='utf-8', newline='\n') as f:
        f.write('# QA 3.3.3.3 replicated dvars: slot<TAB>name<TAB>default (names CS 24+slot, values CS 344+slot; '
                'client net-consts via tools/data/repldvars.py)\n')
        for e in rows:
            f.write('%d\t%s\t%s\n' % (e['i'], e['name'], e['ncs'] if e['ncs'] is not None else ''))
    print('wrote', OUT, len(rows), 'replicated dvars')
