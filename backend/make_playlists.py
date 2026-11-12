"""Build the QA 3.3.3.3 playlist publisher file from the 1.1.13 reference playlist.

    python make_playlists.py <1.1.1.13 install> [--pages 0 1 2 3 4 5]

The QA parser (sub_6111D0) differs from 1.1.13 in what the PVP match list needs:
  catinfo <id> <page> <flag> <name>   defines a category; the PVP UI lists
                                      categories whose <page> matches the page
                                      and whose <flag> is not 2 (sub_6ACAB0)
  playlist <n> ... category <id>      attaches a playlist (n <= 255) to it
The reference has neither, so categories stayed empty and the list was blank.
`buildversion` lines are dropped so no playlist is filtered out by build name.

Each requested page gets its own category ("PVP_p<page>") holding copies of the
core playlists, so whichever label appears in the UI identifies the page id.
Output: publisher/mp_playlists_shipRetail_release.info (served by storage 10/21).
"""
import argparse
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parent
# the reference playlist inside a CoD Online 1.1.1.13 install
REFERENCE = Path('CODOL_Server') / 'lobby' / 'resources' / 'mp_playlists_shipRetail_cb2.info'
OUTPUT = ROOT / 'publisher' / 'mp_playlists_shipRetail_release.info'
# Reference playlist numbers: TDM, Ground War, Kill Confirmed, FFA, S&D, Domination.
CORE = (4, 5, 6, 7, 9, 11)
PLAYLIST_LINE = re.compile(r'^playlist\s+(\d+)', re.IGNORECASE)


def split_blocks(text):
    """-> (header lines, {number: block lines}) splitting at each `playlist N` line."""
    header, blocks, current = [], {}, None
    for line in text.splitlines():
        match = PLAYLIST_LINE.match(line)
        if match:
            current = int(match.group(1))
            blocks[current] = []
        if line.strip().lower().startswith('buildversion'):
            continue
        (blocks[current] if current is not None else header).append(line)
    return header, blocks


# Party-size rules that gate a room's start. 1011 is the numeric dvar name the
# reference pairs with party_minplayers (and 2481 with party_maxplayers).
MIN_PLAYER_RULES = re.compile(r'^rule\s+(party_minplayers|1011|party_matchedplayercount)\s+\d+',
                              re.IGNORECASE)


def solo_block(block, per_team=9):
    """Let one player start the room: min-player rules -> 1, add a per-team cap.

    QA shows a playlist's max from `maxhumanplayersperteam` (absent in 1.1.13,
    hence the "8 ~ 0" player count and "room full but cannot start").
    """
    out = [MIN_PLAYER_RULES.sub(lambda m: f'rule {m.group(1)} 1', line) for line in block]
    out = [line for line in out if not line.strip().lower().startswith('maxhumanplayersperteam')]
    out.insert(1, f'maxhumanplayersperteam {per_team}')
    return out


MAP_LINE = re.compile(r'^(mp_\w+),\w+,\d+\s*$', re.IGNORECASE)


def filter_maps(block, maps):
    """Drop `map,gametype,weight` rotation lines whose map is not in `maps`.

    The client picks its map from the playlist rotation and loads its own
    zone files, so a map the QA build does not ship ends in a fatal
    "cannot find mp_xxx_const.ff" error (seen with mp_cargoship_sh).
    """
    matches = [MAP_LINE.match(line.strip()) for line in block]
    out = [line for line, m in zip(block, matches) if not m or m.group(1) in maps]
    if not any(m and m.group(1) in maps for m in matches):
        # No allowed map in this rotation: keep the playlist playable with one synthesized
        # line (first allowed map, gametype of the original rotation).
        first = next((m for m in matches if m), None)
        if first is None:
            raise ValueError('playlist has no map rotation lines')
        gametype = first.group(0).strip().split(',')[1]
        out.append(f'{sorted(maps)[0]},{gametype},1')
    return out


def build(pages, reference, solo=False, maps=None):
    header, blocks = split_blocks(reference.read_text(encoding='utf-8', errors='replace'))
    if solo:
        blocks = {number: solo_block(block) for number, block in blocks.items()}
    if maps:
        blocks = {number: filter_maps(block, maps) for number, block in blocks.items()
                  if number in CORE}
    out = list(header)
    out.append('')
    for page in pages:
        out.append(f'catinfo {100 + page} {page} 0 PVP_p{page}')
    next_id = 40
    for page in pages:
        for number in CORE:
            block = blocks[number]
            out.append(f'playlist {next_id} // copy of {number} for page {page}')
            out.append(f'category {100 + page}')
            out.extend(block[1:])
            out.append('')
            next_id += 1
    if next_id > 256:
        raise ValueError('QA parser only accepts playlist numbers up to 255')
    return '\n'.join(out) + '\n'


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('c1_install', type=Path, help='your CoD Online 1.1.1.13 install')
    parser.add_argument('--pages', type=int, nargs='+', default=[0, 1, 2, 3, 4, 5])
    parser.add_argument('--solo', action='store_true',
                        help='minimum players 1 so a single local player can start a room')
    parser.add_argument('--maps', nargs='+',
                        help='keep only these maps in every playlist (use maps both builds ship)')
    args = parser.parse_args()
    OUTPUT.parent.mkdir(exist_ok=True)
    OUTPUT.write_text(build(args.pages, args.c1_install / REFERENCE, solo=args.solo, maps=set(args.maps or ())),
                      encoding='utf-8')
    print(f'wrote {OUTPUT} ({OUTPUT.stat().st_size} bytes, pages {args.pages})')


if __name__ == '__main__':
    main()
