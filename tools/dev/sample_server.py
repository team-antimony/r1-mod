"""Sampling profile of a running r1-server (codol_dedrun.exe): where its busiest thread spends time.

    py -3 tools/dev/sample_server.py [seconds] [pid]

The thread with the most CPU time is suspended for an instant every ~1 ms, its instruction pointer and the return
addresses found on its stack are noted, and it is resumed. Samples inside the server's own code are counted per
function (symbols from mingw's nm; the image is relocated, so addresses are taken relative to the module base).
"self" is the function the thread was in; "on stack" counts a function once per sample when it was anywhere on the
stack, which shows which caller the time belongs to. Samples outside the image (sleeping in select, the kernel)
are reported as idle. Read-only apart from the brief suspends.
"""
import bisect, collections, ctypes, ctypes.wintypes as w, subprocess, sys, time

secs = float(sys.argv[1]) if len(sys.argv) > 1 else 10.0
k32 = ctypes.WinDLL('kernel32', use_last_error=True)
psapi = ctypes.WinDLL('psapi')

if len(sys.argv) > 2:
    pid = int(sys.argv[2])
else:
    out = subprocess.run(['tasklist', '/FI', 'IMAGENAME eq codol_dedrun.exe', '/FO', 'CSV', '/NH'],
                         capture_output=True, text=True).stdout.strip().splitlines()
    pid = int(out[0].split(',')[1].strip('"'))
hp = k32.OpenProcess(0x0410, False, pid)
mods = (w.HMODULE * 8)(); need = w.DWORD()
psapi.EnumProcessModulesEx(hp, mods, ctypes.sizeof(mods), ctypes.byref(need), 3)
base = mods[0]
path = ctypes.create_unicode_buffer(520)
psapi.GetModuleFileNameExW(hp, w.HMODULE(base), path, 520)

syms = []
for line in subprocess.run([r'C:\msys64\mingw32\bin\nm.exe', '-n', path.value], capture_output=True, text=True).stdout.splitlines():
    p = line.split()
    if len(p) == 3 and p[1] in 'Tt':
        syms.append((int(p[0], 16) - 0x400000, p[2]))
addrs = [a for a, _ in syms]
text_end = 0x400000 + 0x1000 + 0x37bb38 - 0x400000

def name(rva):
    i = bisect.bisect_right(addrs, rva) - 1
    return syms[i][1] if i >= 0 else '?'

class THREADENTRY32(ctypes.Structure):
    _fields_ = [('dwSize', w.DWORD), ('cntUsage', w.DWORD), ('th32ThreadID', w.DWORD), ('th32OwnerProcessID', w.DWORD),
                ('tpBasePri', w.LONG), ('tpDeltaPri', w.LONG), ('dwFlags', w.DWORD)]

snap = k32.CreateToolhelp32Snapshot(4, 0)
te = THREADENTRY32(); te.dwSize = ctypes.sizeof(te)
best = (0, None)
ok = k32.Thread32First(snap, ctypes.byref(te))
while ok:
    if te.th32OwnerProcessID == pid:
        ht = k32.OpenThread(0x0040 | 0x0008 | 0x0002, False, te.th32ThreadID)
        if ht:
            c = w.FILETIME(); e = w.FILETIME(); kt = w.FILETIME(); ut = w.FILETIME()
            k32.GetThreadTimes(ht, ctypes.byref(c), ctypes.byref(e), ctypes.byref(kt), ctypes.byref(ut))
            cpu = (kt.dwHighDateTime << 32 | kt.dwLowDateTime) + (ut.dwHighDateTime << 32 | ut.dwLowDateTime)
            if cpu > best[0]:
                best = (cpu, te.th32ThreadID)
            k32.CloseHandle(ht)
    ok = k32.Thread32Next(snap, ctypes.byref(te))
tid = best[1]
ht = k32.OpenThread(0x0040 | 0x0008 | 0x0002, False, tid)

class WOW64_CONTEXT(ctypes.Structure):
    _fields_ = [('ContextFlags', w.DWORD), ('Dr', w.DWORD * 6), ('FloatSave', ctypes.c_byte * 112),
                ('SegGs', w.DWORD), ('SegFs', w.DWORD), ('SegEs', w.DWORD), ('SegDs', w.DWORD),
                ('Edi', w.DWORD), ('Esi', w.DWORD), ('Ebx', w.DWORD), ('Edx', w.DWORD), ('Ecx', w.DWORD), ('Eax', w.DWORD),
                ('Ebp', w.DWORD), ('Eip', w.DWORD), ('SegCs', w.DWORD), ('EFlags', w.DWORD), ('Esp', w.DWORD), ('SegSs', w.DWORD),
                ('ExtendedRegisters', ctypes.c_byte * 512)]

ctx = WOW64_CONTEXT()
stack = (w.DWORD * 512)(); got = ctypes.c_size_t()
self_n = collections.Counter(); stack_n = collections.Counter()
total = busy = 0
t0 = time.perf_counter()
while time.perf_counter() - t0 < secs:
    if k32.Wow64SuspendThread(ht) == 0xFFFFFFFF:
        break
    ctx.ContextFlags = 0x10001 | 0x10002  # CONTROL | INTEGER
    okc = k32.Wow64GetThreadContext(ht, ctypes.byref(ctx))
    oks = okc and k32.ReadProcessMemory(hp, ctypes.c_void_p(ctx.Esp), stack, ctypes.sizeof(stack), ctypes.byref(got))
    k32.ResumeThread(ht)
    if okc:
        total += 1
        rva = ctx.Eip - base
        seen = set()
        if 0x1000 <= rva < text_end:
            busy += 1
            self_n[name(rva)] += 1
            seen.add(name(rva))
        if oks:
            for v in stack:
                r = v - base
                if 0x1000 <= r < text_end:
                    seen.add(name(r))
        if 0x1000 <= rva < text_end:  # busy samples only: an idle stack still holds old return addresses
            for n_ in seen:
                stack_n[n_] += 1
    time.sleep(0.001)

print('%d samples of thread %d in %.0f s; %d (%.1f %%) inside the server image' % (total, tid, secs, busy, 100.0 * busy / max(total, 1)))
print('self time (top 25):')
for n_, c in self_n.most_common(25):
    print('  %5.1f %%  %s' % (100.0 * c / max(busy, 1), n_))
print('on stack (top 30; a function is counted once per sample it appears in):')
for n_, c in stack_n.most_common(30):
    print('  %5.1f %%  %s' % (100.0 * c / max(busy, 1), n_))
