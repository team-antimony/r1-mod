import socket
import struct
import time
import unittest

from local_auth.stun import build_dw_discovery_reply, StunServer

# Requests measured from the live QA client on UDP 127.0.0.1:3074 (trace_dwlive.jsonl, 2026-10-29).
IP_DISCOVERY = bytes.fromhex('1e0300')
NAT_DISCOVERY = bytes.fromhex('14020000')
LOOPBACK = bytes([127, 0, 0, 1])


class DiscoveryReplyTests(unittest.TestCase):
    def test_ip_discovery_30_answers_31_with_plain_and_xor_mapped_address(self):
        reply = build_dw_discovery_reply(IP_DISCOVERY, address='127.0.0.1', port=3074)
        self.assertEqual(reply[0], 31)
        self.assertEqual(reply[1:3], IP_DISCOVERY[1:3])
        self.assertEqual(reply[3:7], LOOPBACK)
        self.assertEqual(struct.unpack_from('<H', reply, 7)[0], 3074)
        ip_u32 = int.from_bytes(LOOPBACK, 'little')
        self.assertEqual(struct.unpack_from('<I', reply, 9)[0], ip_u32 ^ 0x2112A442)
        self.assertEqual(struct.unpack_from('<H', reply, 13)[0], 3074 ^ 0x2112)
        self.assertEqual(len(reply), 15)

    def test_nat_discovery_20_answers_21_with_mapped_and_changed_address(self):
        reply = build_dw_discovery_reply(NAT_DISCOVERY, address='127.0.0.1', port=3074)
        self.assertEqual(reply[0], 21)
        self.assertEqual(reply[1:3], NAT_DISCOVERY[1:3])
        self.assertEqual(reply[3:7], LOOPBACK)
        self.assertEqual(struct.unpack_from('<H', reply, 7)[0], 3074)
        self.assertEqual(reply[9:13], LOOPBACK)
        self.assertEqual(struct.unpack_from('<H', reply, 13)[0], 3074)
        self.assertEqual(len(reply), 15)

    def test_keepalive_14_fills_the_address_sentinel(self):
        request = bytes([14, 2, 0]) + bytes(15) + b'\xff\x00\xff\x00' * 2 + b'\x00' * 3
        self.assertEqual(len(request), 29)
        reply = build_dw_discovery_reply(request, address='127.0.0.1', port=3074)
        self.assertEqual(reply[0], 15)
        self.assertEqual(reply[3:18], request[3:18])
        self.assertEqual(reply[18:22], LOOPBACK)
        self.assertEqual(struct.unpack_from('<H', reply, 22)[0], 3074)
        self.assertEqual(reply[24:28], LOOPBACK)

    def test_unknown_or_short_datagrams_are_rejected(self):
        for data in (b'', b'\x1e', bytes([99, 2, 0]), bytes([14, 2, 0])):
            with self.assertRaises(ValueError):
                build_dw_discovery_reply(data, address='127.0.0.1', port=3074)


class DiscoveryServerTests(unittest.TestCase):
    def test_server_answers_discovery_and_survives_connection_reset(self):
        events = []

        def record(kind, **fields):  # same shape as server.main's event()
            events.append((kind, fields))

        server = StunServer(port=0, log=record, name='discovery')
        server.start()
        port = server._socket.getsockname()[1]
        try:
            # A reply to a socket that is already closed makes Windows report
            # WSAECONNRESET on the server's next recvfrom; the server must keep going.
            gone = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            gone.sendto(IP_DISCOVERY, ('127.0.0.1', port))
            gone.close()
            time.sleep(0.2)
            client = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            client.settimeout(5)
            client.sendto(NAT_DISCOVERY, ('127.0.0.1', port))
            data, _ = client.recvfrom(2048)
            self.assertEqual(data[0], 21)
            client.close()
            deadline = time.monotonic() + 2
            while time.monotonic() < deadline and not any(
                    k == 'discovery_response' and f.get('reply_kind') == 'nat_discovery'
                    for k, f in events):
                time.sleep(0.02)
            kinds = [k for k, _ in events]
            self.assertIn('discovery_request', kinds)
            self.assertIn('discovery_response', kinds)
        finally:
            server.stop()


if __name__ == '__main__':
    unittest.main()
