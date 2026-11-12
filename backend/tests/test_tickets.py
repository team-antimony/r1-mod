import base64
import struct
import tempfile
import unittest
from pathlib import Path
from local_auth.profiles import ProfileStore
try:
    from local_auth.tickets import build_auth_response
except ImportError:
    build_auth_response = None


class TicketTests(unittest.TestCase):
    def test_rejects_unsupported_or_malformed_requests(self):
        self.assertIsNotNone(build_auth_response)
        with tempfile.TemporaryDirectory() as directory:
            player = ProfileStore(Path(directory) / 'profiles.db').get_or_create('Tester')
            good = {'auth_task': '10', 'title_id': '1002', 'iv_seed': '42'}
            for field, value in [('auth_task', '29'), ('title_id', '1'),
                                 ('iv_seed', '-1'), ('iv_seed', '4294967296'),
                                 ('iv_seed', True), ('iv_seed', '2.0')]:
                with self.subTest(field=field, value=value), self.assertRaises(ValueError):
                    build_auth_response({**good, field: value}, player, now=1800000000)
            with self.assertRaises(ValueError):
                build_auth_response({}, player)
            with self.assertRaises(ValueError):
                build_auth_response(good, player, now=0xFFFFFFFF)

    def test_qa_task10_response_contains_named_128_byte_ticket(self):
        self.assertIsNotNone(build_auth_response, 'QA ticket issuer is not implemented')
        with tempfile.TemporaryDirectory() as directory:
            player = ProfileStore(Path(directory) / 'profiles.db').get_or_create('LocalPlayer')
            request = {'auth_task': '10', 'title_id': '1002', 'iv_seed': '815856324'}
            response = build_auth_response(request, player, now=1800000000)
            self.assertEqual(response['auth_task'], '11')
            self.assertEqual(response['code'], '700')
            self.assertEqual(response['iv_seed'], request['iv_seed'])
            ticket = base64.b64decode(response['client_ticket'], validate=True)
            self.assertEqual(len(ticket), 128)
            magic, kind, title, issued, expiry, license_id, user_id = struct.unpack_from('<IBIIIQQ', ticket)
            self.assertEqual(magic, 0xEFBDADDE)
            self.assertEqual(title, 1002)
            self.assertEqual(issued, 1800000000)
            self.assertGreater(expiry, issued)
            self.assertEqual(user_id, player.user_id)
            self.assertEqual(ticket[33:97].rstrip(b'\0'), b'LocalPlayer')
            self.assertEqual(ticket[97:121], player.auth_key)
            self.assertEqual(len(base64.b64decode(response['server_ticket'], validate=True)), 128)


if __name__ == '__main__':
    unittest.main()
