import struct
import unittest

from local_auth.frames import parse_args
from local_auth.rooms import (ROOM_PUSH, ROOM_PUSH_MESSAGE, STARTGAME_TYPE_ID, BitWriter,
                              build_room_push, build_startgame_record, common_addr,
                              parse_room_messages, schedule_start, start_countdown)


class BitReader:
    """Mirror of the QA bdBitBuffer reader: LSB-first bits."""

    def __init__(self, data):
        self.value = int.from_bytes(data, 'little')
        self.pos = 0
        self.size = len(data) * 8

    def bits(self, count):
        if self.pos + count > self.size:
            raise ValueError('read past end')
        out = (self.value >> self.pos) & ((1 << count) - 1)
        self.pos += count
        return out

    def blob(self):
        length = self.bits(32)
        return self.bits(8 * length).to_bytes(length, 'little') if length else b''


class CommonAddrTests(unittest.TestCase):
    def test_layout_is_six_slots_plus_nat_type(self):
        addr = common_addr('127.0.0.1', 28960)
        self.assertEqual(len(addr), 37)
        self.assertEqual(addr[0:4], bytes([127, 0, 0, 1]))
        self.assertEqual(struct.unpack_from('<H', addr, 4)[0], 28960)
        self.assertEqual(addr[30:36], addr[0:6])                    # public slot = local slot
        self.assertEqual(addr[6:12], bytes([0, 0xFF, 0, 0xFF, 0, 0]))  # empty-slot sentinel
        self.assertEqual(addr[36], 1)                               # natType open


class StartGameTests(unittest.TestCase):
    def test_record_reads_back_like_sub_6DD500(self):
        session_id, key = bytes(range(8)), bytes(range(16, 32))
        record = build_startgame_record('127.0.0.1', 28960, session_id, key)
        reader = BitReader(record)
        self.assertEqual(reader.bits(1), 0)                         # buffer not type-checked
        self.assertEqual(reader.bits(8), STARTGAME_TYPE_ID)
        self.assertEqual(reader.blob(), common_addr('127.0.0.1', 28960))
        self.assertEqual(reader.blob(), session_id)
        self.assertEqual(reader.blob(), key)
        self.assertLess(reader.size - reader.pos, 8)                # only byte padding left

    def test_rejects_wrong_sizes(self):
        with self.assertRaises(ValueError):
            build_startgame_record('127.0.0.1', 1, b'short', bytes(16))
        with self.assertRaises(ValueError):
            build_startgame_record('127.0.0.1', 1, bytes(8), bytes(15))

    def test_bit_writer_packs_lsb_first(self):
        writer = BitWriter()
        writer.bits(0, 1)
        writer.bits(3, 8)
        self.assertEqual(writer.out(), bytes([0x06, 0x00]))


class ChannelRecordTests(unittest.TestCase):
    def test_matches_bdChatChannelInfo_reader_sub_A55A40(self):
        from local_auth.rooms import build_channel_record
        values = parse_args(build_channel_record(69421, members=1) + b'\x00')
        self.assertEqual(values, [(10, 69421), (8, 1), (8, 0), (10, 69421),
                                  (1, 0), (1, 0), (8, 0)])


class RoomPushTests(unittest.TestCase):
    def test_push_matches_sub_A3D5A0_field_order(self):
        record = build_startgame_record('127.0.0.1', 28960, bytes(8), bytes(16))
        inner_type, app = build_room_push(record, sender_id=69420, sender_name='CODOL', room_id=0)
        self.assertEqual(inner_type, ROOM_PUSH)
        values = parse_args(app + b'\x00')
        self.assertEqual(values[0], (8, ROOM_PUSH_MESSAGE))         # push type 7
        self.assertEqual(values[1], (10, 69420))                     # sender id
        self.assertEqual(values[2], (16, b'CODOL'))                  # sender name
        self.assertEqual(values[3], (10, 0))                         # room / channel id
        self.assertEqual(values[4], (19, struct.pack('<I', len(record)) + record))  # [u32 len][message]
        self.assertEqual(values[5], (8, 0))                          # trailer


class RoomMessageTests(unittest.TestCase):
    # 11/4 args captured from the QA client 10 s after it hosted a matchmaking room (2026-10-31 18:17:58).
    MEMBER = bytes.fromhex('0a2d0f0100000000001308140000001000000002020000001800000020380000000000080100000000')
    START = bytes.fromhex('0a2d0f0100000000001308140000001000000002020000001800000088000000000000080100000000')
    STATE = bytes.fromhex('0a2e0f01000000000013081800000014000000020200000028000000b818000000000000000000080100000000')

    def test_manifold_is_unpacked(self):
        channel, parts = parse_room_messages(self.START)
        self.assertEqual(channel, 69421)
        self.assertEqual([t for t, _ in parts], [17])
        self.assertEqual([t for t, _ in parse_room_messages(self.MEMBER)[1]], [4])
        self.assertEqual([t for t, _ in parse_room_messages(self.STATE)[1]], [23])

    def test_start_countdown(self):
        self.assertEqual(start_countdown(self.START), 0.0)
        self.assertIsNone(start_countdown(self.MEMBER))
        self.assertIsNone(start_countdown(self.STATE))
        self.assertIsNone(start_countdown(b'\x00'))

    @staticmethod
    def countdown_args(seconds):
        """11/4 args holding one StartCountingDown of `seconds`, as the host sends it."""
        writer = BitWriter()
        writer.bits(0, 1)
        writer.bits(17, 8)
        writer.bits(struct.unpack('<I', struct.pack('<f', seconds))[0], 32)
        sub = writer.out()
        manifold = BitWriter()
        manifold.bits(0, 1)
        manifold.bits(1, 8)
        manifold.bits(1, 32)
        manifold.bits(0, 1)
        manifold.bits(len(sub), 32)
        manifold.raw(sub)
        message = manifold.out()
        blob = struct.pack('<I', len(message)) + message
        return (b'\x0a' + struct.pack('<Q', 69421) + b'\x13\x08' + struct.pack('<I', len(blob)) + blob
                + b'\x08' + struct.pack('<I', 1) + b'\x00')

    def test_countdown_seconds_are_read(self):
        self.assertEqual(start_countdown(self.countdown_args(60.0)), 60.0)
        self.assertEqual(start_countdown(self.countdown_args(900.0)), 300.0)
        self.assertEqual(start_countdown(self.countdown_args(float('nan'))), 0.0)

    def test_countdown_cancel_is_negative(self):
        # sub_6E4D30 calls a countdown off with -1; it used to read as 0, i.e. "start now"
        self.assertEqual(start_countdown(self.countdown_args(-1.0)), -1.0)

    def test_schedule_start(self):
        self.assertEqual(schedule_start(None, 0.0, 100.0), 100.0)
        self.assertEqual(schedule_start(None, 60.0, 100.0), 160.0)
        self.assertEqual(schedule_start(160.0, 5.0, 120.0), 125.0)   # a newer countdown replaces the old one
        self.assertIsNone(schedule_start(160.0, -1.0, 120.0))        # cancel
        self.assertIsNone(schedule_start(None, -1.0, 120.0))
        self.assertEqual(schedule_start(160.0, None, 120.0), 160.0)  # message without a countdown


if __name__ == '__main__':
    unittest.main()
