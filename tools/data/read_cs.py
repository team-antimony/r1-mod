import os, sys; sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import fridatmp  # noqa: F401,E402 -- before frida: removes the temp files Frida leaves behind
import frida
dev = frida.get_local_device()
pid = next(p.pid for p in dev.enumerate_processes() if 'codomp_client_shipretail' in p.name.lower())
s = dev.attach(pid)
sc = s.create_script(r"""
const m=Process.enumerateModules().find(m=>m.name.toLowerCase().includes('codomp_client_shipretail'));
const d=m.base.sub(0x400000);
rpc.exports={r(){return {connState:ptr(0x1A2E7EC).add(d).readS32(), csChecksum:ptr(0x2DF7D90).add(d).readU32(), computed:ptr(0x2DF9934).add(d).readU32()};}};
""")
sc.load(); print(sc.exports_sync.r()); s.detach()
