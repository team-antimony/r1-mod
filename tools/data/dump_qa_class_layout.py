"""Write the QA playerdata class layout the native server reads (codol_qaClassLayoutPath).

Source: data/qa_playerdata_schema.json (dumped from the live client by dump_playerdata_def.py). The schema's
offsets table maps each logical leaf to a byte offset in the 32,764-byte playerdata buffer (the uploaded
profile minus its 4-byte prefix). Class records are not a fixed stride (perks[5] and wildcards live in a
separate region), so every leaf is listed explicitly.

Output lines: field<TAB>class<TAB>index<TAB>byte_offset<TAB>size, class -1 for root fields.
Usage: py -3 tools/data/dump_qa_class_layout.py [out.tsv]
"""
import json
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parents[2]
OUT = Path(sys.argv[1]) if len(sys.argv) > 1 else HERE / 'data' / 'qa_weapon_classes.tsv'
SIZES = {0: 4, 1: 1, 9: 2}  # int, byte, short (structured data type kinds)
FIELDS = ('deathstreak', 'killstreaks', 'lethal', 'meleeWpn', 'perks', 'primaryAttach', 'primaryBase',
          'secondaryAttach', 'secondaryBase', 'special', 'tactical')

schema = json.loads((HERE / 'data' / 'qa_playerdata_schema.json').read_text())
offsets = schema['offsets']
arrays = schema['indexed_arrays']
root = {p['name']: p for p in schema['structs'][0]['properties']}
klass = {p['name']: p for p in schema['structs'][2]['properties']}
classes = root['weaponClasses']
count, elem_kind, elem_struct, _ = arrays[classes['type'][1]]
assert elem_kind == 5 and schema['structs'][elem_struct] is schema['structs'][2]
stride = schema['structs'][2]['header'][3]  # logical leaves per class record

lines = ['# QA 3.3.3.3 playerdata (v%d, checksum 0x%08x) class layout: field, class, index, byte offset, size'
         % (schema['header'][0], schema['header'][1])]
cur = root['currentCharacterIndex']
lines.append('currentCharacterIndex\t-1\t0\t%d\t%d' % (offsets[cur['offset']], SIZES[cur['type'][0]]))
for c in range(count):
    base = classes['offset'] + stride * c
    for name in FIELDS:
        p = klass[name]
        kind, sub = p['type']
        if kind == 6:
            n, ekind = arrays[sub][0], arrays[sub][1]
            for i in range(n):
                lines.append('%s\t%d\t%d\t%d\t%d' % (name, c, i, offsets[base + p['offset'] + i], SIZES[ekind]))
        else:
            lines.append('%s\t%d\t0\t%d\t%d' % (name, c, offsets[base + p['offset']], SIZES[kind]))
OUT.write_text('\n'.join(lines) + '\n')
print('%s: %d classes, %d lines' % (OUT, count, len(lines)))
