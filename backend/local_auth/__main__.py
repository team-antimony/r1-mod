"""Local profile administration; protocol serving is a separate component."""
import argparse
import json
from pathlib import Path
from .profiles import ProfileStore


def main():
    parser = argparse.ArgumentParser(description='Offline CODOL local identities')
    sub = parser.add_subparsers(dest='command', required=True)
    profile = sub.add_parser('profile', help='Create or show a persistent local player')
    profile.add_argument('name')
    profile.add_argument('--database', type=Path,
                         default=Path(__file__).resolve().parent.parent / 'data' / 'profiles.sqlite3')
    args = parser.parse_args()
    store = ProfileStore(args.database)
    try:
        player = store.get_or_create(args.name)
    except (ValueError, TypeError) as exc:
        parser.error(str(exc))
    print(json.dumps({'name': player.name, 'user_id': str(player.user_id)}, ensure_ascii=True))


if __name__ == '__main__':
    main()
