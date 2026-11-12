"""Read-only: QA weapon ids that have an allowed-parts row (word_17F8248[128*id]), i.e. the
bases the client builds as composed guns. Writes data\\qa_gun_bases.tsv (id, name, parts).
Leaves out bases whose def names an alternate weapon (def dword 840): composing one makes the
client register a composed alt weapon in the next slot, which shifts every later composed id
("181 %s %d %d" disconnect)."""
import os, sys; sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import fridatmp  # noqa: F401,E402 -- before frida: removes the temp files Frida leaves behind
import frida
from pathlib import Path
DATA = Path(__file__).resolve().parents[2] / 'data'  # repository data folder
dev = frida.get_local_device()
pid = max(p.pid for p in dev.enumerate_processes() if 'codomp_client_shipretail' in p.name.lower())
s = dev.attach(pid)
sc = s.create_script(r'''
const m=Process.enumerateModules().find(m=>m.name.toLowerCase().includes('codomp_client_shipretail'));
const d=m.base.sub(0x400000); function P(a){return ptr(a).add(d);}
function cs(p){ try{ return (p.readCString(48)||'').replace(/[^ -~].*/,''); }catch(e){ return null; } }
rpc.exports={ r(){ const out=[]; for(let id=1; id<289; id++){ let n=0; const row=P(0x17F8248).add(256*id);
    for(let k=0;k<128;k++) if(row.add(2*k).readU16()) n++;
    const def=P(0x17F5A40).add(4*id).readPointer();
    const name=def.isNull()?null:cs(def.readPointer()), alt=def.isNull()?null:cs(def.add(4*840).readPointer());
    out.push([id, name, n, alt]); } return out; }};
''')
sc.load(); r = sc.exports_sync.r(); s.detach()
with open(str(DATA / 'qa_gun_bases.tsv'), 'w', newline='\n') as f:
    for id_, name, n, alt in r:
        if n and not alt: f.write(f'{id_}\t{name}\t{n}\n')
print('with parts:', sum(1 for x in r if x[2]), 'of', len(r), '; left out (alt weapon):',
      [(i, nm, a) for i, nm, n, a in r if n and a])
