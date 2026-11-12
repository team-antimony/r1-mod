"""Extract the Scaleform movies (rawfiles scaleform/*.gfx) from an inflated QA zone and list their AS3 strings.

    uv run python -I tools\\data\\extract_gfx.py <inflated ui_mp.zone> data\\ui

Takes an already-inflated zone that you supply (ui_mp holds the frontend); producing it is not covered here.

Writes <out>\\<name with / -> _> for every movie (ui_mp holds scaleform/default/codo.gfx, the whole frontend),
and next to each one <movie>.strings.txt (the AS3 constant-pool strings of every DoABC tag: class, method and
dvar names) and <movie>.symbols.txt (SymbolClass / ExportAssets names). The movies open in JPEXS FFDec.

Rawfile layout in the zone (IW4): {name ptr, compressed size, size, buffer ptr}, then the inline name, then the
zlib data when the compressed size is non-zero.
"""
import os
import re
import struct
import sys
import zlib


def u30(b, i):
    value = shift = 0
    while True:
        c = b[i]
        i += 1
        value |= (c & 0x7F) << shift
        shift += 7
        if not c & 0x80 or shift >= 35:
            return value, i


def abc_strings(abc):
    """-> the string pool of one ABC block (skips the int, uint and double pools before it)."""
    i = 4
    for skip in (lambda i: u30(abc, i)[1], lambda i: u30(abc, i)[1], lambda i: i + 8):
        n, i = u30(abc, i)
        for _ in range(max(0, n - 1)):
            i = skip(i)
    n, i = u30(abc, i)
    out = []
    for _ in range(max(0, n - 1)):
        length, i = u30(abc, i)
        out.append(abc[i:i + length].decode('utf8', 'replace'))
        i += length
    return out


def movie_strings(data):
    """-> (AS3 strings, symbol names) of an uncompressed GFX/SWF movie."""
    nbits = data[8] >> 3
    p = 8 + (5 + 4 * nbits + 7) // 8 + 4
    strings, symbols = [], []
    while p + 2 <= len(data):
        code_len = struct.unpack_from('<H', data, p)[0]
        p += 2
        code, length = code_len >> 6, code_len & 0x3F
        if length == 0x3F:
            length = struct.unpack_from('<I', data, p)[0]
            p += 4
        body = data[p:p + length]
        p += length
        if code == 82:  # DoABC: flags, name, abc
            strings += abc_strings(body[body.index(b'\0', 4) + 1:])
        elif code == 72:  # DoABC1: abc only
            strings += abc_strings(body)
        elif code in (56, 76):  # ExportAssets, SymbolClass
            q = 2
            for _ in range(struct.unpack_from('<H', body, 0)[0]):
                end = body.index(b'\0', q + 2)
                symbols.append(body[q + 2:end].decode('utf8', 'replace'))
                q = end + 1
        elif code == 0:
            break
    return strings, symbols


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    zone = open(sys.argv[1], 'rb').read()
    out = sys.argv[2]
    os.makedirs(out, exist_ok=True)
    for m in re.finditer(rb'scaleform/[A-Za-z0-9_/.]+\.(?:gfx|swf)\x00', zone):
        name = m.group(0)[:-1].decode()
        _, compressed, size, _ = struct.unpack_from('<IiiI', zone, m.start() - 16)
        body = zone[m.end():m.end() + (compressed if compressed > 0 else size)]
        data = zlib.decompress(body) if compressed > 0 else body
        path = os.path.join(out, name.replace('/', '_'))
        open(path, 'wb').write(data)
        line = '%s: %d bytes, %s v%d' % (name, len(data), data[:3].decode('latin-1'), data[3])
        if data[:3] in (b'GFX', b'FWS'):
            strings, symbols = movie_strings(data)
            open(path + '.strings.txt', 'w', encoding='utf8').write('\n'.join(strings))
            open(path + '.symbols.txt', 'w', encoding='utf8').write('\n'.join(symbols))
            line += ', %d AS3 strings, %d symbols' % (len(strings), len(symbols))
        print(line)


if __name__ == '__main__':
    main()
