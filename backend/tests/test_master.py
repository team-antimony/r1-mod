import socket
import time
import unittest
from local_auth.master import (MasterServer, ServerList, build_getservers_responses, parse_request,
                               SERVERS_PER_PACKET)


class Clock:
    def __init__(self):
        self.now = 0.0

    def __call__(self):
        return self.now


class ServerListTests(unittest.TestCase):
    def test_heartbeat_adds_refreshes_and_flatline_removes(self):
        servers = ServerList()
        self.assertEqual(servers.heartbeat(('127.0.0.1', 28960), 'COD-4'), 'added')
        self.assertEqual(servers.heartbeat(('127.0.0.1', 28960), 'COD-4'), 'refreshed')
        self.assertEqual(servers.live(), [('127.0.0.1', 28960)])
        self.assertEqual(servers.heartbeat(('127.0.0.1', 28960), 'flatline'), 'removed')
        self.assertEqual(servers.live(), [])

    def test_servers_expire_after_ttl(self):
        clock = Clock()
        servers = ServerList(ttl=200, clock=clock)
        servers.heartbeat(('127.0.0.1', 28960), 'COD-4')
        clock.now = 150
        servers.heartbeat(('127.0.0.1', 28975), 'COD-4')
        clock.now = 201
        self.assertEqual(servers.live(), [('127.0.0.1', 28975)])


class ProtocolTests(unittest.TestCase):
    def test_parse_request(self):
        self.assertEqual(parse_request(b'\xff\xff\xff\xffheartbeat COD-4\n'), ('heartbeat', ['COD-4']))
        self.assertEqual(parse_request(b'\xff\xff\xff\xffgetservers 6 full empty'),
                         ('getservers', ['6', 'full', 'empty']))
        self.assertIsNone(parse_request(b'heartbeat COD-4'))
        self.assertIsNone(parse_request(b'\xff\xff\xff\xff\n'))

    def test_empty_list_is_one_packet_with_eot(self):
        self.assertEqual(build_getservers_responses([]),
                         [b'\xff\xff\xff\xffgetserversResponse\n\\EOT\x00\x00\x00'])

    def test_entries_are_ip_and_big_endian_port(self):
        packet, = build_getservers_responses([('127.0.0.1', 28960)])
        self.assertEqual(packet, b'\xff\xff\xff\xffgetserversResponse\n\\\x7f\x00\x00\x01\x71\x20\\EOT\x00\x00\x00')

    def test_long_lists_are_split_and_only_the_last_has_eot(self):
        servers = [('10.0.%d.%d' % (i // 256, i % 256), 28960) for i in range(SERVERS_PER_PACKET + 1)]
        packets = build_getservers_responses(servers)
        self.assertEqual(len(packets), 2)
        self.assertFalse(packets[0].endswith(b'EOT\x00\x00\x00'))
        self.assertTrue(packets[1].endswith(b'\\EOT\x00\x00\x00'))


class MasterServerTests(unittest.TestCase):
    def test_heartbeat_then_getservers_over_udp(self):
        master = MasterServer(port=0)
        master.start()
        game = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        browser = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            game.bind(('127.0.0.1', 0))
            browser.settimeout(2)
            game.sendto(b'\xff\xff\xff\xffheartbeat COD-4\n', master.bind_address)
            for _ in range(100):
                if master.servers.live():
                    break
                time.sleep(0.02)
            browser.sendto(b'\xff\xff\xff\xffgetservers 6 full empty', master.bind_address)
            reply = browser.recv(2048)
            port = game.getsockname()[1]
            self.assertIn(b'\\\x7f\x00\x00\x01' + port.to_bytes(2, 'big'), reply)
            self.assertTrue(reply.endswith(b'\\EOT\x00\x00\x00'))
        finally:
            game.close()
            browser.close()
            master.stop()


if __name__ == '__main__':
    unittest.main()
