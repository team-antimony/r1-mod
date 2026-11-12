import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
import make_playlists  # noqa: E402

REFERENCE = """version 406

gametype dm
name english "Free For All"

playlist 7 league 1 // FFA
name english "Free For All"
ranked
minparty 1
mp_afghan,dm,1
mp_cargoship_sh,dm,1
mp_killhouse_sh,dm,1
mp_rust,dm,1
"""


class MapFilterTests(unittest.TestCase):
    def build(self, **kwargs):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'ref.info'
            path.write_text(REFERENCE, encoding='utf-8')
            old = make_playlists.CORE
            make_playlists.CORE = (7,)
            try:
                return make_playlists.build([0], reference=path, **kwargs)
            finally:
                make_playlists.CORE = old

    def test_without_filter_every_map_is_kept(self):
        text = self.build()
        self.assertIn('mp_cargoship_sh,dm,1', text)
        self.assertIn('mp_rust,dm,1', text)

    def test_filter_keeps_only_allowed_maps(self):
        text = self.build(maps={'mp_killhouse_sh'})
        self.assertIn('mp_killhouse_sh,dm,1', text)
        for gone in ('mp_cargoship_sh', 'mp_afghan', 'mp_rust'):
            self.assertNotIn(gone, text)

    def test_playlist_with_no_allowed_map_gets_a_synthesized_line(self):
        text = self.build(maps={'mp_nuked'})
        self.assertIn('mp_nuked,dm,1', text)           # gametype taken from the original rotation
        for gone in ('mp_cargoship_sh', 'mp_afghan', 'mp_rust', 'mp_killhouse_sh'):
            self.assertNotIn(gone, text)


if __name__ == '__main__':
    unittest.main()
