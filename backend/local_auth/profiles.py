"""Persistent fabricated identities for a loopback-only preservation server.

These profiles are not Tencent accounts. A player name is a local selector,
not a password, and must not be exposed as internet-facing authentication.
"""
from dataclasses import dataclass, field
from pathlib import Path
import secrets
import sqlite3
import unicodedata
from contextlib import closing


@dataclass(frozen=True)
class Profile:
    name: str
    user_id: int
    auth_key: bytes = field(repr=False)


class ProfileStore:
    def __init__(self, path):
        self.path = Path(path)
        self.path.parent.mkdir(parents=True, exist_ok=True)
        with closing(sqlite3.connect(self.path)) as conn, conn:
            conn.execute('CREATE TABLE IF NOT EXISTS profiles '
                         '(name TEXT PRIMARY KEY, user_id INTEGER UNIQUE NOT NULL, '
                         'auth_key BLOB NOT NULL)')

    def get_or_create(self, name):
        if not isinstance(name, str):
            raise TypeError('Player name must be a string')
        name = unicodedata.normalize('NFC', name)
        if any(unicodedata.category(char).startswith('C') for char in name):
            raise ValueError('Control and formatting characters are not allowed')
        name = name.strip()
        if not name or len(name.encode('utf-8')) > 31:
            raise ValueError('Player name must contain 1 to 31 UTF-8 bytes')
        with closing(sqlite3.connect(self.path)) as conn, conn:
            conn.execute('INSERT OR IGNORE INTO profiles VALUES (?, ?, ?)',
                         (name, secrets.randbelow((1 << 63) - 1) + 1,
                          secrets.token_bytes(24)))
            row = conn.execute('SELECT name, user_id, auth_key FROM profiles '
                               'WHERE name = ?', (name,)).fetchone()
            return Profile(*row)
