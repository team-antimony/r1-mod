"""Trace the QA client's snapshot decoder: python trace_delta.py [seconds] [out file]

Attaches to the running QA 3.3.3.3 client and records, for every snapshot it parses, the header (server time,
deltaNum, flags) and every entity MSG_ReadDeltaEntity (0x5A3670) decodes: number, the base's eType, the result's
eType, the bytes consumed and whether the message overran; in a delta snapshot also the decoded entity as hex
(compare_delta.py checks those values against the server's trace). The last snapshots before the client stops (or the
time runs out) are written to the out file (default build/client_delta_trace.log), in the same shape as r1-server's
qa_delta_trace.log (codol_qaDeltaEntities 2), so the two can be compared line by line.
"""
import os, sys; sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import fridatmp  # noqa: F401,E402 -- before frida
import time, frida
from pathlib import Path

seconds = float(sys.argv[1]) if len(sys.argv) > 1 else 60
out = Path(sys.argv[2] if len(sys.argv) > 2 else Path(__file__).resolve().parents[2] / 'build' / 'client_delta_trace.log')
dev = frida.get_local_device()
found = [p for p in dev.enumerate_processes() if 'codomp_client_shipretail' in p.name.lower()]
if len(found) != 1:
    sys.exit(f'{len(found)} client processes')
sess = dev.attach(found[0].pid)
JS = r'''
const main = Process.enumerateModules().find(m => m.name.toLowerCase().includes('codomp_client_shipretail'));
const at = va => ptr(va).add(main.base.sub(0x400000));
let lines = [];
let cur = null;
function flush() { if (lines.length) { send(lines.join('\n')); lines = []; } }
setInterval(flush, 100);
// msg_t: [0] overflowed, [2] data, [3] splitData, [5] cursize, [6] splitSize, [7] readcount, [8] bit
let inDelta = false;
function hex(buf) { const u = new Uint8Array(buf); let o = ''; for (let i = 0; i < u.length; i++) o += (u[i] < 16 ? '0' : '') + u[i].toString(16); return o; }
function pos(m) { return m.add(28).readS32() + ':' + m.add(32).readS32(); }
Interceptor.attach(at(0x46D170), {
  onEnter() {
    const m = this.context.ecx; this.m = m;
    const rc = m.add(28).readS32(), cs = m.add(20).readS32(), data = m.add(8).readPointer();
    let head = '?';
    try {
      inDelta = false;
      if (rc + 6 <= cs) { const p = data.add(rc); head = p.readU32() + ' d' + p.add(4).readU8() + ' f' + p.add(5).readU8(); inDelta = p.add(4).readU8() !== 0; }
      else head = 'split rc=' + rc + ' cs=' + cs;
    } catch (e) { head = 'err ' + e; }
    cur = 'S ' + head + ' at ' + pos(m) + ' size ' + cs + '+' + m.add(24).readS32();
    lines.push(cur);
  },
  onLeave() {
    const m = this.m;
    lines.push('E over=' + m.readS32() + ' at ' + pos(m));
    if (m.readS32()) flush();
  }
});
Interceptor.attach(at(0x5A3670), {
  onEnter(args) {
    this.m = args[0]; this.to = args[3]; this.num = args[4].toInt32();
    this.ft = args[2].add(4).readS32(); this.p0 = pos(args[0]);
  },
  onLeave(ret) {
    const r = ret.toInt32();
    lines.push(' ' + (r === 1 ? 'R' : 'D') + ' ' + this.num + ' from t' + this.ft + ' to t' + (r === 1 ? '-' : this.to.add(4).readS32()) +
               ' ' + this.p0 + '->' + pos(this.m) + (this.m.readS32() ? ' OVER' : ''));
    // in a delta snapshot, the decoded entity (272 bytes) so values can be compared with the server's trace
    if (r !== 1 && inDelta) lines.push('  X ' + this.num + ' ' + hex(this.to.readByteArray(272)));
  }
});
'''
sc = sess.create_script(JS)
got = []
def on_message(msg, data):
    if msg['type'] == 'send':
        got.extend(msg['payload'].split('\n'))
        if len(got) > 400000:
            del got[:100000]
    else:
        got.append('! ' + str(msg.get('description', msg)))
gone = []
sess.on('detached', lambda *a: gone.append(a))
sc.on('message', on_message)
sc.load()
t0 = time.time()
while time.time() - t0 < seconds and not gone:
    time.sleep(0.25)
try:
    sc.unload(); sess.detach()
except Exception:
    pass
out.write_text('\n'.join(got) + '\n', encoding='ascii', errors='replace')
print(f'{len(got)} lines -> {out}; client {"gone: " + str(gone[0][0]) if gone else "still running"}')
