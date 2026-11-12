import tempfile
import unittest
from pathlib import Path

try:
    from local_auth.profiles import ProfileStore
except ImportError:
    ProfileStore = None


class ProfileTests(unittest.TestCase):
    def test_names_are_normalized_and_unsafe_names_are_rejected(self):
        self.assertIsNotNone(ProfileStore)
        with tempfile.TemporaryDirectory() as directory:
            store = ProfileStore(Path(directory) / 'profiles.sqlite3')
            self.assertEqual(store.get_or_create('  Caf\u00e9  '),
                             store.get_or_create('Cafe\u0301'))
            for name in ('', '   ', 'bad\nname', 'bad\x00name', '\u202eevil', 'x' * 32, '\u754c' * 11):
                with self.subTest(name=repr(name)), self.assertRaises(ValueError):
                    store.get_or_create(name)
            with self.assertRaises(TypeError):
                store.get_or_create(123)

    def test_cli_creates_a_named_profile_without_printing_key(self):
        import json
        import subprocess
        import sys
        with tempfile.TemporaryDirectory() as directory:
            command = [sys.executable, '-m', 'local_auth', 'profile', 'LocalPlayer',
                       '--database', str(Path(directory) / 'profiles.sqlite3')]
            first = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(first.returncode, 0, first.stderr)
            data = json.loads(first.stdout)
            self.assertEqual(set(data), {'name', 'user_id'})
            self.assertEqual(data['name'], 'LocalPlayer')
            second = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(second.returncode, 0, second.stderr)
            self.assertEqual(json.loads(second.stdout), data)

    def test_name_has_stable_local_identity_across_reopen(self):
        self.assertIsNotNone(ProfileStore, 'local profile store is not implemented')
        with tempfile.TemporaryDirectory() as directory:
            db = Path(directory) / 'profiles.sqlite3'
            first = ProfileStore(db).get_or_create('player')
            second = ProfileStore(db).get_or_create('player')
            self.assertEqual(first, second)
            self.assertEqual(first.name, 'player')
            self.assertGreater(first.user_id, 0)
            self.assertLess(first.user_id, 1 << 63)
            self.assertEqual(first.auth_key, second.auth_key)
            self.assertEqual(len(first.auth_key), 24)


if __name__ == '__main__':
    unittest.main()
