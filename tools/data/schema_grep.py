"""List rank/emblem-like properties in the dumped QA playerdata schemas with struct index and logical offsets."""
import json
from pathlib import Path
import re
import sys
DATA = Path(__file__).resolve().parents[2] / 'data'  # repository data folder

pattern = re.compile(sys.argv[1] if len(sys.argv) > 1 else
                     r'rank|experience|prestige|emblem|icon|title|callsign|clan|badge|avatar|skin|portrait|name', re.I)
for f in (str(DATA / 'qa_securedata_schema.json'), str(DATA / 'qa_playerdata_schema.json')):
    r = json.load(open(f, encoding='utf8'))
    print('==', f.rsplit('\\', 1)[-1], 'version', r['header'][0], 'size', r['header'][12],
          'structs', len(r['structs']), 'enums', len(r['enums']), 'offsets', len(r['offsets']))
    for si, st in enumerate(r['structs']):
        for p in st['properties']:
            if pattern.search(p['name']):
                print('  struct', si, p)
    print('  root type', r['header'][10:12])
