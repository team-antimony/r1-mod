"""Read-only: dump the client's weapon part tables (mp/coweapitems/<cat>.csv, index-0 rows) to
data/qa_weapon_parts.tsv as weapon<TAB>item<TAB>tag<TAB>viewmodel."""
import os, sys; sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import fridatmp  # noqa: F401,E402 -- before frida: removes the temp files Frida leaves behind
import frida, sys
from pathlib import Path
DATA = Path(__file__).resolve().parents[2] / 'data'  # repository data folder
CATS=['muz','mag','sto','gri','iro','rai','bip','sco','sra','cqc','bas']
names=['mp/coweapitems/%s.csv'%c for c in CATS]
dev = frida.get_local_device()
pid = max(p.pid for p in dev.enumerate_processes() if 'codomp_client_shipretail' in p.name.lower())
s = dev.attach(pid)
try:
    sc = s.create_script(r'''
const m=Process.enumerateModules().find(m=>m.name.toLowerCase().includes('codomp_client_shipretail'));
const d=m.base.sub(0x400000); function P(a){return ptr(a).add(d);}
rpc.exports={ r(names){ const out=[]; const buckets=new Int32Array(P(0x1BC9F20).readByteArray(4*80000)); const ents=P(0x1FC4E28);
  for(let b=0;b<80000;b++){ let i=buckets[b], hops=0; while(i && hops++<64){ const e=ents.add(28*i); if(e.readS32()==37){ const hdr=e.add(8).readPointer(); let nm=''; try{ nm=(hdr.readPointer().readCString()||'').toLowerCase(); }catch(x){}
      if(names.indexOf(nm)>=0){ const cols=hdr.add(4).readS32(), rows=hdr.add(8).readS32(), vals=hdr.add(12).readPointer();
        for(let r=0;r<rows;r++){ const cell=c=>{ const sp=vals.add(8*(r*cols+c)).readPointer(); return sp.isNull()?'':(sp.readCString()||''); };
          if(cols>=6 && cell(2)=='0') out.push([cell(1), cell(0), cell(5), cell(4)]); } } } i=e.add(20).readS32(); } }
  return out; }};
''')
    sc.load(); rows=sc.exports_sync.r(names)
finally:
    s.detach()
rows.sort()
with open(str(DATA / 'qa_weapon_parts.tsv'),'w',encoding='utf-8',newline='\n') as f:
    f.write('# QA 3.3.3.3 weapon parts per weapon (client mp/coweapitems/<cat>.csv index-0 rows; r1-mod tools/data/dumpparts.py): weapon<TAB>item<TAB>tag<TAB>viewmodel\n')
    for r in rows: f.write('\t'.join(r)+'\n')
print(len(rows))
