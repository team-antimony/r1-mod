import struct
import socket
import unittest
from local_auth.stun import build_binding_response, build_qa_probe_reply, StunServer


def make_request(txid=b'\x01\x02\x03\x04\x05\x06\x07\x08\x09\x0a\x0b\x0c\x0d\x0e\x0f\x10'):
    return struct.pack('>HH', 0x0001, 4) + txid + b'\x00\x00\x00\x00'


class StunTests(unittest.TestCase):
    def test_binding_response_maps_source_and_echoes_transaction(self):
        response = build_binding_response(make_request(), '127.0.0.1', 3074)
        rtype, rlen = struct.unpack_from('>HH', response)
        self.assertEqual(rtype, 0x0101)
        self.assertEqual(response[4:20], b'\x01\x02\x03\x04\x05\x06\x07\x08\x09\x0a\x0b\x0c\x0d\x0e\x0f\x10')
        atype, alen = struct.unpack_from('>HH', response, 20)
        self.assertEqual((atype, alen), (0x0001, 8))
        self.assertEqual(response[24:26], b'\x00\x01')
        self.assertEqual(struct.unpack_from('>H', response, 26)[0], 3074)
        self.assertEqual(response[28:32], socket.inet_aton('127.0.0.1'))
        self.assertEqual(rlen % 4, 0)

    # Captured 2026-10-29 from the real QA client talking to the original STUN host
    # (116.130.229.213:8000): the client accepted this 74-byte reply.
    LIVE_REQUEST = bytes.fromhex('75720010000800030000000b0000723b0000006400180000')
    LIVE_REPLY = bytes.fromhex(
        '75720010003a00040000000b0000723b000000000000001800000001000000103131362e3133302e'
        '3232392e3231330000001f400000000e3235352e3235352e3235352e300000000000')

    def test_qa_probe_reply_matches_live_server_byte_for_byte(self):
        reply = build_qa_probe_reply(self.LIVE_REQUEST, address='116.130.229.213', port=8000)
        self.assertEqual(reply, self.LIVE_REPLY)

    def test_qa_probe_reply_advertises_loopback_by_default(self):
        reply = build_qa_probe_reply(self.LIVE_REQUEST)
        self.assertEqual(struct.unpack_from('>H', reply, 4)[0], len(reply) - 16)
        self.assertEqual(struct.unpack_from('>H', reply, 6)[0], 4)
        self.assertEqual(reply[12:16], self.LIVE_REQUEST[12:16])
        self.assertIn(b'127.0.0.1\x00', reply)
        self.assertEqual(reply[16:28], self.LIVE_REPLY[16:28])
        self.assertEqual(reply[28:32], struct.pack('>I', len(b'127.0.0.1\x00')))

    def test_qa_probe_reply_rejects_non_probes(self):
        with self.assertRaises(ValueError):
            build_qa_probe_reply(b'\x00\x01' + b'\x00' * 22)
        with self.assertRaises(ValueError):
            build_qa_probe_reply(b'\x75\x72\x00')

    def test_non_binding_request_is_rejected(self):
        with self.assertRaises(ValueError):
            build_binding_response(struct.pack('>HH', 0x0003, 0) + b'\x00' * 16, '127.0.0.1', 1)
        with self.assertRaises(ValueError):
            build_binding_response(b'\x00' * 8, '127.0.0.1', 1)

    def test_server_answers_datagrams(self):
        events = []

        def record(kind, **fields):  # same shape as server.main's event()
            events.append((kind, fields))

        server = StunServer(port=0, log=record)
        server.start()
        port = server._socket.getsockname()[1]
        import socket
        client = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        client.settimeout(5)
        client.sendto(make_request(), ('127.0.0.1', port))
        data, _ = client.recvfrom(2048)
        self.assertEqual(struct.unpack_from('>H', data)[0], 0x0101)
        # The reply datagram can beat the server thread's log append; poll briefly.
        import time
        deadline = time.monotonic() + 2
        kinds = [k for k, _ in events]
        while 'stun_response' not in kinds and time.monotonic() < deadline:
            time.sleep(0.02)
            kinds = [k for k, _ in events]
        self.assertIn('stun_request', kinds)
        self.assertIn('stun_response', kinds)
        response_event = dict(events)['stun_response']
        self.assertEqual(response_event['reply_kind'], 'rfc3489')
        self.assertEqual(response_event['request_hex'], make_request().hex())
        self.assertEqual(response_event['response_hex'], data.hex())
        server.stop()
        client.close()


if __name__ == '__main__':
    unittest.main()