"""Read-only: dump QA weapon timing fields (plain defs) -> data/qa_weapon_times.tsv."""
import os, sys; sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import fridatmp  # noqa: F401,E402 -- before frida: removes the temp files Frida leaves behind
import frida, sys
from pathlib import Path
DATA = Path(__file__).resolve().parents[2] / 'data'  # repository data folder
FIELDS=[('iFireTime',3344),('iFireDelay',1776),('iRechamberTime',1792),('iRechamberBoltTime',1800),
        ('iDropTime',1868),('iRaiseTime',1872),('iAltDropTime',1876),('quickDropTime',1880),('quickRaiseTime',1884),
        ('iFirstRaiseTime',3404),('iEmptyRaiseTime',1892),('iEmptyDropTime',1896),
        ('iReloadTime',1820),('iReloadEmptyTime',1832),('iReloadAddTime',1840),('iReloadStartTime',1856),
        ('iReloadStartAddTime',1860),('iReloadEndTime',1864),('iMeleeDelay',1780),('meleeChargeDelay',1784),
        ('iMeleeTime',1812),('meleeChargeTime',1816),('iAltRaiseTime',1888),
        ('fireType',1164),   # sub_424830: 1/5 single shot per pull, 2..4 burst size, 0 full auto
        ('sidearmClass',1152)]   # sub_4236A0: 5 = switching TO this gun is a quick drop (pistols, machine pistols)
dev = frida.get_local_device()
pid = max(p.pid for p in dev.enumerate_processes() if 'codomp_client_shipretail' in p.name.lower())
s = dev.attach(pid)
try:
    sc = s.create_script(r'''
const m=Process.enumerateModules().find(m=>m.name.toLowerCase().includes('codomp_client_shipretail'));
const d=m.base.sub(0x400000); function P(a){return ptr(a).add(d);}
rpc.exports={ r(offs){ const out=[]; for(let id=1;id<900;id++){ const def=P(0x17F5A40).add(4*id).readPointer(); if(def.isNull()) continue;
  let nm=''; try{ nm=def.readPointer().readCString(64); }catch(e){ continue; } if(!nm || nm[0]=='*') continue;
  const mm=nm.match(/^[A-Za-z0-9_]*/)[0]; out.push([id, mm].concat(offs.map(o=>def.add(o).readS32()))); } return out; }};
''')
    sc.load(); rows=sc.exports_sync.r([o for _,o in FIELDS])
finally:
    s.detach()
with open(str(DATA / 'qa_weapon_times.tsv'),'w',encoding='utf-8',newline='\n') as f:
    f.write('# QA 3.3.3.3 weapon timings (ms) read from the client WeaponDefs (r1-mod tools/data/dumptimes.py); offsets: '+' '.join('%s=%d'%x for x in FIELDS)+'\n')
    f.write('id\tname\t'+'\t'.join(n for n,_ in FIELDS)+'\n')
    for r in rows: f.write('\t'.join(str(x) for x in r)+'\n')
print(len(rows))
