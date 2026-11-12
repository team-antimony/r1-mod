"""Dump the QA client's mp/playerdata.def (StructuredDataDef) from the running client to JSON.

Layout (read from the 2.6.9.7 schema dump and the QA asset): def = {version, checksum, enumCount, enums*,
structCount, structs*, indexedArrayCount, indexedArrays*, enumedArrayCount, enumedArrays*, rootType{type,index},
size, offsetCount, offsets*, ?}. Needs the client running with the asset loaded (it is by the lobby).
Usage: uv run --with frida python dump_playerdata_def.py [out.json]
"""
import os, sys; sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import fridatmp  # noqa: F401,E402 -- before frida: removes the temp files Frida leaves behind
import frida, json, sys
from pathlib import Path
DATA = Path(__file__).resolve().parents[2] / 'data'  # repository data folder
out = sys.argv[1] if len(sys.argv) > 1 else str(DATA / 'qa_securedata_schema.json')
dev = frida.get_local_device()
pid = next(p.pid for p in dev.enumerate_processes() if 'codomp_client_shipretail' in p.name.lower())
s = dev.attach(pid)
sc = s.create_script(r"""
const m=Process.enumerateModules().find(m=>m.name.toLowerCase().includes('codomp_client_shipretail'));
const d=m.base.sub(0x400000); const at=v=>ptr(v).add(d);
const Req=new NativeFunction(at(0x49E7D0),'pointer',['int','pointer','int'],'mscdecl');
const u=(p,i)=>p.add(4*i).readU32();
const s32=(p,i)=>p.add(4*i).readS32();
function str(p){try{return ptr(p).readCString();}catch(e){return null;}}
function dump(name){
  const a=Req(39,Memory.allocUtf8String(name),1); const def=a.add(8).readPointer();
  const R={name:name, def:def.toString(), header:[], enums:[], structs:[], indexed_arrays:[], offsets:[]};
  for(let i=0;i<16;i++) R.header.push(u(def,i));
  const enumCount=u(def,2), enums=ptr(u(def,3));
  for(let i=0;i<enumCount;i++){const e=enums.add(12*i); const n=u(e,0), ents=ptr(u(e,2)); const L=[];
    for(let k=0;k<n;k++){const en=ents.add(8*k); L.push({name:str(u(en,0)), index:en.add(4).readU16()});}
    R.enums.push({header:[u(e,0),u(e,1)], entries:L});}
  const structCount=u(def,4), structs=ptr(u(def,5));
  for(let i=0;i<structCount;i++){const st=structs.add(20*i); const n=u(st,0), props=ptr(u(st,1)); const L=[];
    for(let k=0;k<n;k++){const pr=props.add(24*k); L.push({name:str(u(pr,0)), type:[u(pr,1),u(pr,2)], offset:u(pr,3), extra:[u(pr,4),u(pr,5)]});}
    R.structs.push({header:[u(st,0),u(st,2),u(st,3),u(st,4)], properties:L});}
  const iaCount=u(def,6), ia=ptr(u(def,7));
  for(let i=0;i<iaCount;i++){const x=ia.add(16*i); R.indexed_arrays.push([u(x,0),u(x,1),u(x,2),u(x,3)]);}
  const offCount=u(def,13), offs=ptr(u(def,14));
  for(let i=0;i<offCount && i<20000;i++){R.offsets.push(u(offs,i));}
  return R;}
rpc.exports={dump:dump};
""")
sc.load()
r = sc.exports_sync.dump('mp/playersecuredata.def')
r['header_hex'] = [hex(x) for x in r['header']]
json.dump(r, open(out, 'w', encoding='utf8'), ensure_ascii=False)
print('written', out)
print('version', r['header'][0], 'checksum', hex(r['header'][1]), 'size', r['header'][12])
print('enums', len(r['enums']), 'structs', [(len(x['properties']), x['header']) for x in r['structs']], 'indexed arrays', len(r['indexed_arrays']), 'offsets', len(r['offsets']))
for st in r['structs']:
    print(' struct props sample:', [p['name'] for p in st['properties'][:8]])
s.detach()

