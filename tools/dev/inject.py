"""Inject r1-mod.dll into a running client: python inject.py [dll] [process name or pid]

Without a process, the running QA client (codoMP_client_shipRetail*.exe). Name matching ignores case and
accepts a part of the name.
"""
import os, sys; sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import fridatmp  # noqa: F401,E402 -- before frida: removes the temp files Frida leaves behind
import sys, frida
from pathlib import Path

dll = Path(sys.argv[1] if len(sys.argv) > 1 else Path(__file__).resolve().parents[2] / 'build' / 'bin' / 'Win32' / 'Release' / 'r1-mod.dll')
target = sys.argv[2] if len(sys.argv) > 2 else 'codomp_client_shipretail'
dev = frida.get_local_device()
if target.isdigit():
    pid = int(target)
else:
    found = [p for p in dev.enumerate_processes() if target.lower() in p.name.lower()]
    if len(found) != 1:
        sys.exit(f'{len(found)} processes match "{target}": ' + ', '.join(f'{p.name} ({p.pid})' for p in found))
    pid = found[0].pid
sess = dev.attach(pid)
sc = sess.create_script(r'''
// 'propagate': a fault inside DllMain belongs to the DLL's own handlers (frida would take it by default)
const LoadLibraryA = new NativeFunction(Process.getModuleByName('kernel32.dll').getExportByName('LoadLibraryA'), 'pointer', ['pointer'], { exceptions: 'propagate' });
rpc.exports = { load(path) { return LoadLibraryA(Memory.allocAnsiString(path)).toString(); } };
''')
sc.load()
print('LoadLibrary ->', sc.exports_sync.load(str(dll.resolve())))
sess.detach()
