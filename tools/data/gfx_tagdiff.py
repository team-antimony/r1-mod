"""Compare two GFX/SWF movies tag by tag, to check that an FFDec edit changed only the classes it should.

    uv run python -I tools\\data\\gfx_tagdiff.py data\\ui\\scaleform_default_codo.gfx data\\ui\\codo_mod.gfx

Prints the header of each file and every tag whose code or body differs (DoABC tags with their class name).
FFDec cannot parse three Scaleform PlaceObject3 tags in codo.gfx; this confirms they are written back unchanged.
"""
import hashlib
import struct
import sys


def tags(path):
    data = open(path, 'rb').read()
    nbits = data[8] >> 3
    p = 8 + (5 + 4 * nbits + 7) // 8 + 4
    out = []
    while p < len(data):
        code_len = struct.unpack_from('<H', data, p)[0]
        p += 2
        code, length = code_len >> 6, code_len & 0x3F
        if length == 0x3F:
            length = struct.unpack_from('<I', data, p)[0]
            p += 4
        body = data[p:p + length]
        p += length
        name = body[4:body.index(b'\0', 4)].decode('utf8', 'replace') if code == 82 else ''
        out.append((code, name, hashlib.sha1(body).hexdigest(), length))
        if code == 0:
            break
    return data[:4], len(data), out


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    (magic_a, size_a, a), (magic_b, size_b, b) = tags(sys.argv[1]), tags(sys.argv[2])
    print('%s: %r %d bytes, %d tags' % (sys.argv[1], magic_a, size_a, len(a)))
    print('%s: %r %d bytes, %d tags' % (sys.argv[2], magic_b, size_b, len(b)))
    if len(a) != len(b):
        print('tag count differs; listing stops at the shorter file')
    diff = [(i, x, y) for i, (x, y) in enumerate(zip(a, b)) if x != y]
    print('%d differing tags' % len(diff))
    for i, x, y in diff:
        print('  #%d: code %d %s %d bytes -> code %d %s %d bytes' % (i, x[0], x[1], x[3], y[0], y[1], y[3]))


if __name__ == '__main__':
    main()
