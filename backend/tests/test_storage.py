import struct
import tempfile
import unittest
from pathlib import Path

from local_auth.frames import parse_args, build_task_reply, w_u32
from local_auth.storage import publisher_file_reply, PUBLISHER_FILE_TASK

# 10/21 arguments as the live QA client sent them (server_stun.log, 2026-10-29).
PLAYLIST_ARGS = bytes.fromhex(
    '100010' + 'mp_playlists_shipRetail_release.info'.encode().hex() + '0000')


class ArgParserTests(unittest.TestCase):
    def test_parses_the_live_publisher_file_request(self):
        self.assertEqual(parse_args(PLAYLIST_ARGS),
                         [(0x10, b''), (0x10, b'mp_playlists_shipRetail_release.info')])

    def test_parses_every_scalar_tag_and_blob(self):
        data = (bytes([3, 7]) + bytes([8]) + struct.pack('<I', 9) + bytes([10]) + struct.pack('<Q', 11)
                + bytes([1, 1]) + bytes([19, 8]) + struct.pack('<I', 2) + b'ab' + bytes([0]))
        self.assertEqual(parse_args(data), [(3, 7), (8, 9), (10, 11), (1, 1), (19, b'ab')])

    def test_rejects_truncated_or_unknown_tags(self):
        for data in (bytes([8, 1, 2]), bytes([16]) + b'no-terminator', bytes([0x7f])):
            with self.assertRaises(ValueError):
                parse_args(data)


class PublisherFileTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def test_missing_file_is_sent_as_an_empty_blob(self):
        results, note = publisher_file_reply(PLAYLIST_ARGS, self.root)
        self.assertEqual(results, (bytes([19]) + w_u32(0),))
        self.assertEqual(note['file'], 'mp_playlists_shipRetail_release.info')
        self.assertEqual(note['source'], 'empty')

    def test_existing_file_is_sent_verbatim(self):
        (self.root / 'configs_s.info').write_bytes(b'set g_speed 190\n')
        args = bytes.fromhex('100010') + b'configs_s.info\x00\x00'
        results, note = publisher_file_reply(args, self.root)
        self.assertEqual(results, (bytes([19]) + w_u32(16) + b'set g_speed 190\n',))
        self.assertEqual((note['source'], note['bytes']), ('file', 16))

    def test_path_traversal_names_are_never_read(self):
        (self.root.parent / 'secret.info').write_bytes(b'nope')
        args = bytes.fromhex('100010') + b'../secret.info\x00\x00'
        results, note = publisher_file_reply(args, self.root)
        self.assertEqual(results, (bytes([19]) + w_u32(0),))
        self.assertEqual(note['source'], 'refused')
        (self.root.parent / 'secret.info').unlink()

    def test_reply_is_one_counted_result(self):
        results, _ = publisher_file_reply(PLAYLIST_ARGS, self.root)
        _, body = build_task_reply(PUBLISHER_FILE_TASK, transaction_id=5, results=results)
        # u64 txn, u32 error, u8 task, u32 count=1, u32 total=1, then the blob
        self.assertEqual(body[14:16], bytes([3, PUBLISHER_FILE_TASK]))
        self.assertEqual(body[16:26], w_u32(1) + w_u32(1))
        self.assertEqual(body[26:], bytes([19]) + w_u32(0))


if __name__ == '__main__':
    unittest.main()
