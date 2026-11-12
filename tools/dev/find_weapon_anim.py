"""Find which animation of a weapon definition names a clip, in the running QA client (memory read only).

    py -3 tools/dev/find_weapon_anim.py <weapon> <clip>      e.g. m4a1tech_mp viewmodel_m4a1tech_inspect

Looks both assets up in the client's asset table (buckets 0x1BC9F20, entries of 7 dwords at 0x1FC4E28: [0] type,
[2] header, byte +16 loaded, [5] next; hash as weapon_anims.c loaded_xanim), then searches the weapon's header and
the blocks it points to for the clip's XAnimParts pointer or its name pointer, and prints each place as an offset and
as an index into a pointer array starting at the first anim found.
"""
import ctypes, ctypes.wintypes as w, struct, subprocess, sys

weapon, clip = sys.argv[1], sys.argv[2]
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
    b = read(addr, 128)
    return b.split(b'\0')[0].decode('latin1') if b else ''


def va(a):
    return a - 0x400000 + exe


def lookup(name, types):
    found = []
    for t in types:
        hsh = t
        for c in name:
            hsh = (ord('/') if c == '\\' else ord(c.lower())) + 31 * hsh
            hsh &= 0xFFFFFFFF
        i = u32(va(0x1BC9F20) + 4 * (hsh % 0x13880))
        guard = 0
        while i and guard < 4096:
            e = va(0x1FC4E28) + 28 * i
            et, hdr = u32(e), u32(e + 8)
            if et == t and hdr and cstr(u32(hdr)).lower() == name.lower():
                found.append((t, hdr, (read(e + 16, 1) or b'\0')[0]))
            i = u32(e + 20)
            guard += 1
    return found


clips = lookup(clip, [2])
weps = lookup(weapon, range(0, 64))
print('clip %s: %s' % (clip, ['type %d header 0x%x loaded %d' % c for c in clips]))
print('weapon %s: %s' % (weapon, ['type %d header 0x%x loaded %d' % c for c in weps]))
if not clips or not weps:
    sys.exit(0)
parts = clips[0][1]
name_ptr = u32(parts)
want = {parts: 'XAnimParts', name_ptr: 'name'}
for wt, hdr, _ in weps:
    block = read(hdr, 0x400) or b''
    print('weapon header type %d at 0x%x' % (wt, hdr))
    for off in range(0, len(block) - 3, 4):
        v = struct.unpack_from('<I', block, off)[0]
        if v in want:
            print('  header+0x%x = %s' % (off, want[v]))
        # one level of indirection: arrays of pointers
        if v and 0x10000 < v < 0x7FFF0000 and off < 0x200:
            arr = read(v, 4 * 80)
            if not arr:
                continue
            for k in range(80):
                p = struct.unpack_from('<I', arr, 4 * k)[0]
                if p in want:
                    first = struct.unpack_from('<I', arr, 0)[0]
                    print('  header+0x%x -> [%d] = %s   (array at 0x%x; [0] names %r, [1] %r)'
                          % (off, k, want[p], v, cstr(u32(first)) or cstr(first),
                             cstr(u32(struct.unpack_from('<I', arr, 4)[0])) or cstr(struct.unpack_from('<I', arr, 4)[0])))
