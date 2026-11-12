import hashlib
import hmac
import struct
import unittest
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
from local_auth.lobby import Keys
try:
    from local_auth.frames import decode_client_data, encode_server_data, decode_server_data
except ImportError:
    decode_client_data = encode_server_data = decode_server_data = None


class DataFrameTests(unittest.TestCase):
    def test_client_frame_checks_direction_counter_mac_and_padding(self):
        self.assertIsNotNone(decode_client_data, 'Encrypted QA data codec is missing')
        keys = Keys(b'A' * 20, b'B' * 20, b'C' * 16, b'D' * 16)
        app = b'\x47\x03hello'
        plaintext = struct.pack('<I', len(app)) + b'\x86' + app
        plaintext += bytes((-len(plaintext)) % 16)
        iv = bytes(range(16))
        cipher = Cipher(algorithms.AES(keys.c2s_aes), modes.CBC(iv)).encryptor()
        encrypted = cipher.update(plaintext) + cipher.finalize()
        prefix = struct.pack('<I', 30 + len(encrypted)) + b'\xab\x85' + struct.pack('<I', 1) + iv + encrypted
        frame = prefix + hmac.digest(keys.c2s_mac, prefix, 'sha1')[:8]
        self.assertEqual(decode_client_data(frame, keys, expected_counter=1), (0x86, app))
        with self.assertRaisesRegex(ValueError, 'counter'):
            decode_client_data(frame, keys, expected_counter=2)
        with self.assertRaisesRegex(ValueError, 'MAC'):
            decode_client_data(frame[:-1] + bytes([frame[-1] ^ 1]), keys, expected_counter=1)
        wrong = Keys(keys.s2c_mac, keys.c2s_mac, keys.s2c_aes, keys.c2s_aes)
        with self.assertRaisesRegex(ValueError, 'MAC'):
            decode_client_data(frame, wrong, expected_counter=1)


class ServerFrameTests(unittest.TestCase):
    def test_server_reply_round_trips_and_uses_receive_keys(self):
        self.assertIsNotNone(encode_server_data, 'Server reply encoder is missing')
        keys = Keys(b'A' * 20, b'B' * 20, b'C' * 16, b'D' * 16)
        frame = encode_server_data(0x01, b'\x47\x03result payload', keys, send_counter=1)
        self.assertEqual(struct.unpack_from('<I', frame)[0], len(frame) - 4)
        self.assertEqual(frame[4:6], b'\xab\x85')
        self.assertEqual(struct.unpack_from('<I', frame, 6)[0], 1)
        self.assertEqual(frame[-8:], hmac.digest(keys.s2c_mac, frame[:-8], 'sha1')[:8])
        kind, app = decode_server_data(frame, keys, expected_counter=1)
        self.assertEqual(kind, 0x01)
        self.assertEqual(app, b'\x47\x03result payload')
        with self.assertRaisesRegex(ValueError, 'counter'):
            decode_server_data(frame, keys, expected_counter=2)


if __name__ == '__main__':
    unittest.main()
