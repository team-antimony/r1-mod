"""Build the QA seed playerdata buffer (default classes + tutorial flags).

The QA layout was read from the live client (`dump_playerdata_def.py`): the flattened offsets table puts the class
fields at the same absolute offsets as 2.6.9.7 (class stride 114 bytes, notification flags from bit 35001), so the
2.6.9.7 "local-profile-loadout" recipe applies. Item ids come from the client's own `mp/coMarketplaceItems.csv`
(`dump_stringtable.py`). Output: data\\qa_playerdata.bin (32,764 bytes, the live buffer size).
"""
import csv, json, struct, sys
from pathlib import Path
DATA = Path(__file__).resolve().parents[2] / 'data'  # repository data folder

BUF = 32764
OUT = str(DATA / 'qa_playerdata.bin')
ITEMS = str(DATA / 'tables' / 'mp_coMarketplaceItems.csv')
NOTIFY_BASE_BIT = 35001
NOTIFY = {  # flag -> index in mp/uiNotifications.csv (same indices as 2.6.9.7, checked against the QA table)
    'un_mp_tutorial': 12, 'un_mp_tutorial_video': 14, 'un_onboard_tutorial': 16, 'un_mp_onboard': 17,
    'un_zm_tutorial': 18, 'un_onboard_intro_video': 20, 'un_sp_onboard': 21, 'un_zm_onboard': 22,
    'onboard_cargoship': 84, 'onboard_shooting_range': 85, 'onboard_cqb_course': 86, 'onboard_complete': 87,
    'un_career': 92, 'un_loadout': 93, 'onboard_character_selected': 96, 'onboard_gamemode_summary': 99,
    'onboard_daily_challenge_tour_viewed': 100,
}
CLASS_BASE, CLASS_STRIDE = 4392, 114
NAME_BASE, NAME_STRIDE, NAME_LEN = 8689, 21, 21
REL = {'characterIndex': 0, 'lethal': 22, 'meleeWpn': 26, 'primaryBase': 74, 'secondaryBase': 102, 'tactical': 110}
CHARACTER = 545       # cha_INFbaseA in the QA item table


def load_items():
    return {r[0]: r for r in csv.reader(open(ITEMS, encoding='utf-8')) if r}


def pick_primaries(items):
    """One base (non-skin) weapon per class family, in a fixed order."""
    want = ['ast', 'smg', 'lmg', 'snp', 'shg']
    found = {}
    for r in items.values():
        if len(r) > 7 and r[2] == 'wea' and r[3] in want and r[3] not in found and r[1].endswith('_mp') \
                and r[7] == '0' and r[1].count('_') == 1:
            found[r[3]] = int(r[0])
    return [found[k] for k in want if k in found], found


def main():
    items = load_items()
    for need in ('10035', '60000', '60004', '60014', str(CHARACTER)):
        assert need in items, f'item {need} missing from the QA table'
    buf = bytearray(BUF)
    log = []

    def put_u8(off, v, why):
        buf[off] = v & 0xFF; log.append((off, 1, v, why))

    def put_u16(off, v, why):
        struct.pack_into('<H', buf, off, v); log.append((off, 2, v, why))

    def put_u32(off, v, why):
        struct.pack_into('<I', buf, off, v); log.append((off, 4, v, why))

    for name, idx in NOTIFY.items():
        bit = NOTIFY_BASE_BIT + idx
        buf[bit >> 3] |= 1 << (bit & 7)
        log.append((bit >> 3, 'bit%d' % (bit & 7), 1, name))
    put_u32(3858, CHARACTER, 'characterClasses.0.slots.0')
    put_u8(4146, 0, 'currentCharacterIndex')
    put_u32(9590, CHARACTER, 'onboardCharacterID')

    primaries, families = pick_primaries(items)
    label = ['Assault', 'SMG', 'LMG', 'Sniper', 'Shotgun']
    for i, prim in enumerate(primaries):
        base = CLASS_BASE + CLASS_STRIDE * i
        put_u16(base + REL['characterIndex'], 0, f'weaponClasses.{i}.characterIndex')
        put_u32(base + REL['primaryBase'], prim, f'weaponClasses.{i}.primaryBase')
        put_u32(base + REL['secondaryBase'], 10035, f'weaponClasses.{i}.secondaryBase')
        put_u32(base + REL['lethal'], 60000, f'weaponClasses.{i}.lethal')
        put_u32(base + REL['tactical'], 60004, f'weaponClasses.{i}.tactical')
        put_u32(base + REL['meleeWpn'], 60014, f'weaponClasses.{i}.meleeWpn')
        nm = ('Offline %d %s' % (i + 1, label[i]))[:NAME_LEN - 1].encode('ascii') + b'\0'
        off = NAME_BASE + NAME_STRIDE * i
        buf[off:off + len(nm)] = nm
        log.append((off, len(nm), nm.decode(), f'weaponClasses.{i}.name'))
    open(OUT, 'wb').write(buf)
    json.dump({'families': families, 'writes': [[str(a), str(b), str(c), d] for a, b, c, d in log]},
              open(OUT + '.json', 'w'), indent=1)
    print('wrote', OUT, len(buf), 'bytes,', len(log), 'fields; primaries', families)


if __name__ == '__main__':
    sys.exit(main())
