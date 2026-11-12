import unittest
try:
    from local_auth.lobby import make_server_header, verify_client_auth
except ImportError:
    make_server_header = verify_client_auth = None

P = bytes.fromhex('c8000000c8000000d2000000d2000000000008002e7360dafd16fae7')


def client_body(title=1002, seed=0, ticket=bytes(128)):
    # Independent fixture writer: bit offsets from exact-build analysis.
    fields = [(0, 8, 7), (8, 1, 1), (9, 5, 8), (14, 32, title),
              (46, 5, 8), (51, 32, seed), (83, 1024, int.from_bytes(ticket, 'little'))]
    result = bytearray(139)
    for offset, width, value in fields:
        for bit in range(width):
            if value & (1 << bit):
                result[(offset + bit) // 8] |= 1 << ((offset + bit) % 8)
    return bytes(result)


class HandshakeTests(unittest.TestCase):
    def test_malformed_negotiation_and_auth_body_are_rejected(self):
        self.assertIsNotNone(make_server_header)
        from local_auth.profiles import Profile
        for initial in (P[:-1], b'\0' * 28, P[:16] + b'\0' * 4 + P[20:]):
            with self.subTest(initial=initial.hex()), self.assertRaises(ValueError):
                make_server_header(initial, server_id=bytes(8), nonce=bytes(8))
        header = make_server_header(P, server_id=bytes(8), nonce=bytes(8))
        player = Profile('Test', 1, bytes(range(24)))
        lookup = lambda token: {'profile': player, 'expires': 200, 'iv_seed': 0, 'title_id': 1002}
        valid = bytes.fromhex('95000000ab82') + client_body() + bytes.fromhex('c9e0d96c734499b1')
        malformed = bytearray(valid)
        malformed[6] = 0
        with self.assertRaisesRegex(ValueError, 'body'):
            verify_client_auth(P, header, bytes(malformed), lookup, now=100)
        with self.assertRaises(ValueError):
            verify_client_auth(P, header, valid[:-1] + b'\0', lookup, now=100)
        with self.assertRaisesRegex(ValueError, 'expired'):
            verify_client_auth(P, header, valid, lookup, now=201)

    def test_fragmented_tcp_handshake_establishes_a_local_session(self):
        import base64
        import hashlib
        import hmac
        import socket
        import struct
        import tempfile
        import threading
        import time
        from pathlib import Path
        import local_auth.lobby as lobby
        from local_auth.server import make_server
        from local_auth.profiles import ProfileStore
        self.assertTrue(hasattr(lobby, 'make_lobby_server'), 'TCP handshake service is not implemented')
        with tempfile.TemporaryDirectory() as directory:
            player = ProfileStore(Path(directory) / 'profiles.db').get_or_create('SocketTest')
            auth = make_server(player, port=0)
            token = b'T' * 128
            auth.sessions[token] = {'profile': player, 'expires': time.time() + 300,
                                    'iv_seed': 42, 'title_id': 1002}
            events = []
            server = lobby.make_lobby_server(auth, port=0, event=lambda kind, **kw: events.append(kind))
            thread = threading.Thread(target=server.serve_forever, daemon=True)
            thread.start()
            def read_exact(conn, length):
                result = b''
                while len(result) < length:
                    chunk = conn.recv(length - len(result))
                    self.assertTrue(chunk, 'Premature TCP close')
                    result += chunk
                return result
            try:
                with socket.create_connection(server.server_address, timeout=3) as conn:
                    conn.sendall(P[:3]); conn.sendall(P[3:])
                    header = read_exact(conn, 26)
                    prefix = bytes.fromhex('95000000ab82') + client_body(seed=42, ticket=token)
                    th = hashlib.sha1(P[8:] + header + prefix).digest()
                    prk = hmac.digest(th, player.auth_key, 'sha1')
                    challenge = hmac.digest(prk, b'CLIENTCHAL\x01', 'sha1')[:16]
                    packet = prefix + challenge[:8]
                    conn.sendall(packet[:19]); conn.sendall(packet[19:])
                    response = read_exact(conn, 14)
                    self.assertEqual(response, bytes.fromhex('0a000000ab83') + challenge[8:])
                    # Send a real AES/HMAC application frame after the handshake.
                    from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
                    previous = b''; material = b''
                    for counter in range(1, 5):
                        previous = hmac.digest(prk, previous + b'BDDATA' + bytes([counter]), 'sha1')
                        material += previous
                    plain = (struct.pack('<I', 4) + b'\x86' + bytes([71, 3, 4, 0]))
                    plain += bytes((-len(plain)) % 16)
                    iv = bytes(16)
                    cipher = Cipher(algorithms.AES(material[40:56]), modes.CBC(iv)).encryptor()
                    encrypted = cipher.update(plain) + cipher.finalize()
                    prefix = struct.pack('<I', 30 + len(encrypted)) + b'\xab\x85' + struct.pack('<I', 1) + iv + encrypted
                    frame = prefix + hmac.digest(material[:20], prefix, 'sha1')[:8]
                    conn.sendall(frame)
                    for _ in range(50):
                        if 'lobby_service_received' in events:break
                        time.sleep(.01)
                    self.assertIn('lobby_service_received', events)
                self.assertIn('lobby_authenticated', events)
            finally:
                server.shutdown(); server.server_close(); thread.join(3)
                auth.server_close()

    def test_header_and_proofs_match_static_reference_vector(self):
        self.assertIsNotNone(make_server_header, 'QA lobby handshake is not implemented')
        from local_auth.profiles import Profile
        header = make_server_header(P, server_id=bytes(8), nonce=bytes(8))
        self.assertEqual(header.hex(), '16000000ab81d200000000000000000000000000000000000000')
        packet = bytes.fromhex('95000000ab82') + client_body() + bytes.fromhex('c9e0d96c734499b1')
        player = Profile('VectorTest', 1, bytes(range(24)))
        lookup = lambda token: {'profile': player, 'expires': 200, 'iv_seed': 0, 'title_id': 1002} if token == bytes(128) else None
        reply, keys, profile = verify_client_auth(P, header, packet, lookup, now=100)
        self.assertEqual(reply.hex(), '0a000000ab83a2793099a8a2bfe2')
        self.assertEqual(keys.c2s_mac.hex(), '96c0700290f791c4dc2c87edd2eabd542b619323')
        self.assertEqual(keys.s2c_mac.hex(), '14a87ea7bcab3fe0c08c5b8e33e85fd7b5ad6e55')
        # Report line wraps are not byte/key boundaries: concatenate first.
        expected_data = bytes.fromhex(
            '96c0700290f791c4dc2c87edd2eabd542b619323'
            '14a87ea7bcab3fe0c08c5b8e33e85fd7b5ad6e55'
            '178d505fc426ac71d9b323786748173d5'
            '78d3806830cfe81ef3428b1911cd101')
        self.assertEqual(keys.c2s_aes, expected_data[40:56])
        self.assertEqual(keys.s2c_aes, expected_data[56:72])
        self.assertEqual(profile, player)


if __name__ == '__main__':
    unittest.main()
