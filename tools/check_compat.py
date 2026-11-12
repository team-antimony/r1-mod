"""Check that src/common/compat.h and docs/COMPATIBILITY.md give the same compatibility number.

    python tools/check_compat.py

Exits 1 when they differ or either cannot be read (CI runs it).
"""
import pathlib
import re
import sys

root = pathlib.Path(__file__).resolve().parent.parent
header = (root / 'src' / 'common' / 'compat.h').read_text(encoding='utf-8')
doc = (root / 'docs' / 'COMPATIBILITY.md').read_text(encoding='utf-8')

h = re.search(r'^#define R1_COMPAT (\d+)\s*$', header, re.M)
d = re.search(r'^\*\*r1_compat: (\d+)\*\*\s*$', doc, re.M)
if not h or not d:
    print('check_compat: could not find the number in %s' % ('compat.h' if not h else 'COMPATIBILITY.md'))
    sys.exit(1)
releases = re.findall(r'^\| (\d+) \|', doc, re.M)
if h.group(1) != d.group(1):
    print('check_compat: compat.h says %s, COMPATIBILITY.md says %s' % (h.group(1), d.group(1)))
    sys.exit(1)
if h.group(1) not in releases:
    print('check_compat: COMPATIBILITY.md has no release row for r1_compat %s' % h.group(1))
    sys.exit(1)
print('check_compat: r1_compat %s' % h.group(1))
