import hashlib
import struct
import unittest

from local_auth.dtls import (QaAssociation, decrypt_record, derive_key, encrypt_record,
                             record_mac)

# Captured 2026-10-29 from the live QA client (127.0.0.1:30001 -> game server) with the
# shared ECDH x-coordinate and session key read from sh1x's codol_sessions.txt / the lobby log.
X = bytes.fromhex('d8bb703464fa2343e38fb65db187fe6143c9627bb5f381f02172b8aa')
SESSION_KEY = bytes.fromhex('f6c2f8a890d1f697d73c4f8417d3cc43')
LOCAL_TAG = 60289
GETCHALLENGE_RECORD = bytes.fromhex(
    '060281eb0100cf81e71086fb1c342600'
    '92371c2922da9157eb5abd96de285ff749a7111738f41ac083c508c1e5c626567e4281414324')
KEEPALIVE_RECORD = bytes.fromhex('060281eb0200' + '0000000000000000' + '0200' + '23b8')


class KeyTests(unittest.TestCase):
    def test_key_is_sha1_of_x_and_session_key(self):
        self.assertEqual(derive_key(X, SESSION_KEY),
                         bytes.fromhex('ffb510549907ec19fe4a83349dd4617867768484'))


class RecordTests(unittest.TestCase):
    def setUp(self):
        self.key = derive_key(X, SESSION_KEY)

    def test_live_getchallenge_record_authenticates_and_decrypts(self):
        counter, plain = decrypt_record(GETCHALLENGE_RECORD, self.key, LOCAL_TAG)
        self.assertEqual(counter, 1)
        self.assertTrue(plain.startswith(b'\xff\xff\xff\xffgetchallenge 0 "'))
        self.assertEqual(len(plain), 38)

    def test_tampering_is_rejected(self):
        bad = bytearray(GETCHALLENGE_RECORD)
        bad[20] ^= 1
        with self.assertRaises(ValueError):
            decrypt_record(bytes(bad), self.key, LOCAL_TAG)

    def test_wrong_tag_is_rejected(self):
        with self.assertRaises(ValueError):
            decrypt_record(GETCHALLENGE_RECORD, self.key, LOCAL_TAG + 1)

    def test_mac_covers_header_length_and_ciphertext(self):
        record = GETCHALLENGE_RECORD
        self.assertEqual(record_mac(self.key, record[:6], record[14:16], record[16:]), record[6:14])

    def test_encrypt_then_decrypt_roundtrip(self):
        for size in (0, 1, 16, 17, 100):
            plain = bytes(range(size))
            record = encrypt_record(plain, self.key, counter=7, tag=LOCAL_TAG)
            self.assertEqual(decrypt_record(record, self.key, LOCAL_TAG), (7, plain))

    def test_counter_extends_past_16_bits(self):
        record = encrypt_record(b'hello', self.key, counter=70000, tag=LOCAL_TAG)
        self.assertEqual(decrypt_record(record, self.key, LOCAL_TAG, last_counter=69990), (70000, b'hello'))


class AssociationTests(unittest.TestCase):
    def test_association_sends_with_peer_tag_and_increments_counter(self):
        association = QaAssociation(X, SESSION_KEY, local_tag=1111, peer_tag=2222)
        first = association.seal(b'abc')
        second = association.seal(b'def')
        self.assertEqual(struct.unpack_from('<H', first, 2)[0], 2222)
        self.assertEqual(struct.unpack_from('<H', first, 4)[0], 1)
        self.assertEqual(struct.unpack_from('<H', second, 4)[0], 2)

    def test_two_ends_interoperate(self):
        client = QaAssociation(X, SESSION_KEY, local_tag=1111, peer_tag=2222)
        server = QaAssociation(X, SESSION_KEY, local_tag=2222, peer_tag=1111)
        self.assertEqual(server.open(client.seal(b'getchallenge')), b'getchallenge')
        self.assertEqual(client.open(server.seal(b'challengeResponse')), b'challengeResponse')

    def test_replayed_record_is_rejected(self):
        client = QaAssociation(X, SESSION_KEY, local_tag=1111, peer_tag=2222)
        server = QaAssociation(X, SESSION_KEY, local_tag=2222, peer_tag=1111)
        record = client.seal(b'once')
        server.open(record)
        with self.assertRaises(ValueError):
            server.open(record)


if __name__ == '__main__':
    unittest.main()
