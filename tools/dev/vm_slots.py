"""List the clips in chosen view-model animation slots of the running QA client (memory read only).

    py -3 tools/dev/vm_slots.py [slot ...]        default: 16 19 20 50 51 52 53

View-model anim sets as weapon_anims.c reads them: table 0x1A19510[*0x7817BB8] ^ 0x39641402, 24 bytes per view
model, +4 -> -> anim set, clip pointer at +8 + 8 * slot, a clip's name pointer at +0. The client must be in a match.
"""
import ctypes, ctypes.wintypes as w, struct, subprocess, sys, collections

slots = [int(a) for a in sys.argv[1:]] or [16, 19, 20, 50, 51, 52, 53]
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
got = ctypes.c_size_t()


def read(addr, n):
    buf = ctypes.create_string_buffer(n)
    if not addr or not k32.ReadProcessMemory(h, ctypes.c_void_p(addr), buf, n, ctypes.byref(got)) or got.value != n:
        return None
    return buf.raw


def u32(addr):
    b = read(addr, 4)
    return struct.unpack('<I', b)[0] if b else 0


def cstr(addr):
    b = read(addr, 96)
    return b.split(b'\0')[0].decode('latin1') if b else ''


def va(a):
    return a - 0x400000 + exe


if u32(va(0x1A2E7EC)) != 10:
    sys.exit('the client is not in a match')
base = u32(va(0x1A19510) + 4 * u32(va(0x7817BB8))) ^ 0x39641402
models = 0
filled = collections.Counter()
shown = 0
for vm in range(2048):
    pp = u32(base + 24 * vm + 4)
    anims = u32(pp) if pp else 0
    if not anims:
        continue
    models += 1
    idle = cstr(u32(u32(anims + 8 + 8 * 1)))
    row = []
    for s in slots:
        clip = u32(anims + 8 + 8 * s)
        name = cstr(u32(clip)) if clip else ''
        if name:
            filled[s] += 1
        row.append('%d=%s' % (s, name or '-'))
    if any('=-' not in r for r in row) and shown < 40:
        shown += 1
        print('vm %4d idle %-36s %s' % (vm, idle, '  '.join(row)))
print('%d view models; slots filled: %s' % (models, ', '.join('%d: %d' % (s, filled[s]) for s in slots)))
