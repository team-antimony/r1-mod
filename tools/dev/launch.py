"""Start the QA 3.3.3.3 client with the loopback redirects and inject r1-mod.dll.

    uv run --with frida python tools/dev/launch.py [--dll build/bin/Win32/Release/r1-mod.dll] --game <game_dir> [--seconds 3600]

The redirects (first STUN probe and the decrypted STUN config -> 127.0.0.1) are needed until r1-mod.dll
replaces them natively. The local Demonware backend (backend/) must already be running.
"""
import argparse, json, threading, time
from pathlib import Path

import os, sys; sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import fridatmp  # noqa: F401,E402 -- before frida: removes the temp files Frida leaves behind
import frida

HERE = Path(__file__).resolve().parents[2]

AGENT = r'''
const main=Process.enumerateModules().find(m=>m.name.toLowerCase().includes('codomp_client_shipretail'));
const delta=main.base.sub(0x400000);
const at=va=>ptr(va).add(delta);
const ws2=Process.getModuleByName('ws2_32.dll');
Interceptor.attach(ws2.getExportByName('connect'),{onEnter(a){
 const s=a[1]; if(s.readU16()!==2)return;
 if(s.add(4).readU8()===116&&s.add(5).readU8()===130){s.add(4).writeU8(127);s.add(5).writeU8(0);s.add(6).writeU8(0);s.add(7).writeU8(1);send({event:'connect_redirect'});}
}});
let redirected=false;
Interceptor.attach(at(0xB166D0),{onEnter(){
 if(redirected)return; redirected=true;
 try{const a1=this.context.ebx;const n=a1.add(392).readU32();
  for(let i=0;i<n&&i<16;i++){const e=a1.add(396+i*36);const s=e.readCString();if(s&&s!=='127.0.0.1')e.writeUtf8String('127.0.0.1');}}catch(e){}
}});
const live=at(0xCFAFE0); let last=null;
setInterval(()=>{try{const o=live.readPointer();if(!o.isNull()){const v=o.add(500).readS32();if(v!==last){last=v;send({event:'live_state',value:v});}}}catch(e){}},250);
Interceptor.attach(at(0x597D20),{onEnter(a){let t=null;try{t=a[1].readCString();}catch(e){}
 let s2=null,n3=null;try{s2=a[2].readCString();}catch(e){}try{n3=a[3].toInt32();}catch(e){}
 send({event:'com_error',code:a[0].toInt32(),text:t,arg2:s2,arg3:n3,bt:Thread.backtrace(this.context,Backtracer.ACCURATE).map(p=>'0x'+p.sub(delta).toString(16)).slice(0,8)});}});
const LoadLibraryA=new NativeFunction(Process.getModuleByName('kernel32.dll').getExportByName('LoadLibraryA'),'pointer',['pointer']);
rpc.exports={load(path){return LoadLibraryA(Memory.allocAnsiString(path)).toString();}};
send({event:'ready',base:main.base.toString()});
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--dll', default=str(HERE / 'build' / 'bin' / 'Win32' / 'Release' / 'r1-mod.dll'))
    parser.add_argument('--game', type=Path, required=True, help='the unpacked QA 3.3.3.3 game folder')
    parser.add_argument('--exe', default='codoMP_client_shipRetail_loosefs_v2.exe')
    parser.add_argument('--seconds', type=float, default=3600.0)
    parser.add_argument('--skip-onboarding', action='store_true',
                        help='set dvar 3233 and switch to the main menu once r1-mod is loaded')
    args = parser.parse_args()

    live = threading.Event()
    done = threading.Event()

    def on_message(message, _data):
        payload = message.get('payload', message)
        print(json.dumps(payload), flush=True)
        if payload.get('event') == 'live_state' and payload.get('value') == 2:
            live.set()

    device = frida.get_local_device()
    pid = device.spawn([str(args.game / args.exe), '-q', '0', '-src=tgp', '-game_id', '29',
                        '-zone_id', '83886082'], cwd=str(args.game),
                       env=fridatmp.ENV)    # the game must not inherit the temp folder fridatmp removes
    session = device.attach(pid)
    session.on('detached', lambda *a: done.set())
    script = session.create_script(AGENT)
    script.on('message', on_message)
    script.load()
    device.resume(pid)

    injected = False
    deadline = time.monotonic() + args.seconds
    while not done.is_set() and time.monotonic() < deadline:
        if live.is_set() and not injected:
            time.sleep(3)
            print('LoadLibrary ->', script.exports_sync.load(str(Path(args.dll).resolve())), flush=True)
            injected = True
            if args.skip_onboarding:
                # same override qa_console.py uses: dvar "3233" is read by IsOnboardComplete
                time.sleep(12)   # r1-mod waits for the engine's command list to settle first
                inbox = Path(args.dll).resolve().parent / 'r1-mod_in.txt'
                with inbox.open('a', encoding='utf8') as f:
                    f.write('set 3233 1\n')
                time.sleep(2)
                with inbox.open('a', encoding='utf8') as f:
                    f.write('switchtomainmenu\n')
                print('onboarding skipped', flush=True)
        done.wait(0.5)
    session.detach()


if __name__ == '__main__':
    main()

