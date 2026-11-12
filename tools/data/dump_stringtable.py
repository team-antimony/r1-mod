"""Dump a StringTable asset (type 37) from the running QA client to CSV.
Usage: uv run --with frida python dump_stringtable.py mp/botClassTable.csv [out.csv]
"""
import os, sys; sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import fridatmp  # noqa: F401,E402 -- before frida: removes the temp files Frida leaves behind
import csv, frida, sys
from pathlib import Path
DATA = Path(__file__).resolve().parents[2] / 'data'  # repository data folder
name = sys.argv[1]
out = sys.argv[2] if len(sys.argv) > 2 else str(DATA / 'tables' / name.replace('/', '_'))
import os; os.makedirs(os.path.dirname(out), exist_ok=True)
dev = frida.get_local_device()
pid = next(p.pid for p in dev.enumerate_processes() if 'codomp_client_shipretail' in p.name.lower())
s = dev.attach(pid)
sc = s.create_script(r"""
const m=Process.enumerateModules().find(m=>m.name.toLowerCase().includes('codomp_client_shipretail'));
const d=m.base.sub(0x400000);
const Req=new NativeFunction(ptr(0x49E7D0).add(d),'pointer',['int','pointer','int'],'mscdecl');
rpc.exports={dump(name){const a=Req(37,Memory.allocUtf8String(name),1); if(a.isNull()) return null;
  const cols=a.add(4).readS32(), rows=a.add(8).readS32(), vals=a.add(12).readPointer();
  const out=[]; for(let r=0;r<rows;r++){const row=[]; for(let c=0;c<cols;c++){const p=vals.add(8*(r*cols+c)).readPointer(); let t=''; try{t=p.isNull()?'':p.readUtf8String();}catch(e){} row.push(t);} out.push(row);}
  return {cols:cols, rows:rows, data:out};}};
""")
sc.load()
r = sc.exports_sync.dump(name)
if r is None: print('asset not found'); sys.exit(1)
with open(out, 'w', encoding='utf-8', newline='') as f:
    csv.writer(f).writerows(r['data'])
print(name, 'cols', r['cols'], 'rows', r['rows'], '->', out)
for row in r['data'][:6]: print('  ', row[:14])
s.detach()
