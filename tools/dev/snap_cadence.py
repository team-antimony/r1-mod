"""Snapshot cadence as the QA client sees it: how evenly snapshots arrive and how far server time moves in each.

    py -3 tools/dev/snap_cadence.py [seconds]

Reads the running client's memory only (no injection): cl = *0x1A2E838 ^ 0x6891D1C7 (the pointer CL_AdjustTimeDelta
0x4FA5B0 uses), cl+0x3538 = the newest snapshot's serverTime, cl+0x3580 = serverTimeDelta. The value is polled in a
tight loop and every change is stamped with the performance counter, so the arrival interval is good to ~0.05 ms.
An even server shows arrival intervals of 1000/sv_fps ms with a small spread and one serverTime step size.
"""
import ctypes, ctypes.wintypes as w, struct, subprocess, sys, time

secs = float(sys.argv[1]) if len(sys.argv) > 1 else 15.0
out = subprocess.run(['tasklist', '/FI', 'IMAGENAME eq codoMP_client_shipRetail_loosefs_v2.exe', '/FO', 'CSV', '/NH'],
                     capture_output=True, text=True).stdout.strip().splitlines()
if not out or ',' not in out[0]:
    sys.exit('the QA client is not running')
pid = int(out[0].split(',')[1].strip('"'))
k32 = ctypes.WinDLL('kernel32', use_last_error=True)
h = k32.OpenProcess(0x0410, False, pid)
psapi = ctypes.WinDLL('psapi')
mods = (w.HMODULE * 8)(); need = w.DWORD()
psapi.EnumProcessModulesEx(h, mods, ctypes.sizeof(mods), ctypes.byref(need), 3)
exe = mods[0] or 0x400000
buf = ctypes.create_string_buffer(4); got = ctypes.c_size_t()
rpm = k32.ReadProcessMemory

def u32(addr):
    if not rpm(h, ctypes.c_void_p(addr), buf, 4, ctypes.byref(got)) or got.value != 4:
        return None
    return struct.unpack('<I', buf.raw)[0]

cl = u32(0x1A2E838 - 0x400000 + exe) ^ 0x6891D1C7
state = u32(0x1A2E7EC - 0x400000 + exe)
if state != 10:
    sys.exit('the client is not in a match (connection state %s)' % state)
addr = cl + 0x3538
last = u32(addr); t_last = None
arrivals = []; steps = []
t0 = time.perf_counter()
while True:
    now = time.perf_counter()
    if now - t0 >= secs:
        break
    v = u32(addr)
    if v is not None and v != last:
        if t_last is not None:
            arrivals.append((now - t_last) * 1000.0)
            steps.append((v - last) & 0xFFFFFFFF)
        t_last = now; last = v

if len(arrivals) < 10:
    sys.exit('only %d snapshots seen' % len(arrivals))
a = sorted(arrivals); n = len(a)
mean = sum(a) / n
sd = (sum((x - mean) ** 2 for x in a) / n) ** 0.5
pct = lambda p: a[min(n - 1, int(n * p))]
print('%d snapshots in %.0f s: %.1f per second' % (n, secs, n / secs))
print('arrival interval ms: mean %.2f  sd %.2f  min %.2f  p50 %.2f  p95 %.2f  p99 %.2f  max %.2f'
      % (mean, sd, a[0], pct(0.5), pct(0.95), pct(0.99), a[-1]))
hist = {}
for x in arrivals:
    b = int(round(x / 4.0)) * 4
    hist[b] = hist.get(b, 0) + 1
print('arrival histogram (4 ms bins): ' + '  '.join('%d ms: %d' % (b, hist[b]) for b in sorted(hist)))
sh = {}
for s in steps:
    sh[s] = sh.get(s, 0) + 1
print('serverTime step per snapshot: ' + '  '.join('%d ms: %d' % (s, sh[s]) for s in sorted(sh)[:12]))
late = sum(1 for x in arrivals if x > 1.5 * pct(0.5))
print('snapshots more than 1.5x the median late: %d (%.1f %%)' % (late, 100.0 * late / n))
