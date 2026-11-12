"""Room messages for the QA 3.3.3.3 client: the start-game push.

Delivery (lobby -> client), all exact-build:
  * inner type 2 frame (push, sub_A42DB0) carrying tagged u32 push type 7
    (room message, sub_A3D5A0): u64 sender id, string sender name (<= 64),
    u64 room/channel id, blob message (<= 1024), u32 trailer;
  * the message blob is a bit buffer (sub_6EABE0 -> sub_A3A130(..., 1)): the first
    bit is the buffer's type-check flag (0 = no type tags), then an 8-bit typeId.

dwRoomMessageStartGame (typeId 3, reader sub_6DD500) holds three blobs, each a
32-bit length then the bytes: host bdCommonAddr (<= 255, 37 in this build),
session id (8) and the client security key (16). On receipt, dwRoom case 3
(sub_6E8F10) copies the MatchMakingInfo into the room and connects to the host
through sub_6DF750 -- if the room accepts it (not already connecting,
g_connState == 0).
"""
import socket
import struct

from .frames import TAG_BLOB, TAG_STRING, TAG_UINT32, TAG_UINT64, w_blob, w_u32, w_u64

ROOM_PUSH = 0x02
ROOM_PUSH_MESSAGE = 7
STARTGAME_TYPE_ID = 3
EMPTY_ADDR_SLOT = bytes([0x00, 0xFF, 0x00, 0xFF, 0x00, 0x00])


class BitWriter:
    """LSB-first bit packer matching bdBitBuffer."""

    def __init__(self):
        self.value = 0
        self.size = 0

    def bits(self, value, count):
        self.value |= (value & ((1 << count) - 1)) << self.size
        self.size += count

    def raw(self, data):
        self.bits(int.from_bytes(data, 'little'), 8 * len(data))

    def blob(self, data):
        self.bits(len(data), 32)
        self.raw(data)

    def out(self):
        return self.value.to_bytes((self.size + 7) // 8, 'little')


def common_addr(ip, port, slots=6, nat_type=1):
    """bdCommonAddr: `slots` x {IPv4, u16 port}, empty slots as the 00FF00FF0000 sentinel,
    then a NAT-type byte (1 = open). Slot 0 (local) and the last slot (public) are the host."""
    address = bytearray(EMPTY_ADDR_SLOT * slots)
    entry = socket.inet_aton(ip) + struct.pack('<H', port)
    for slot in (0, slots - 1):
        address[slot * 6:slot * 6 + 6] = entry
    return bytes(address) + bytes([nat_type])


def build_startgame_record(ip, port, session_id, security_key):
    """dwRoomMessageStartGame pointing the client at ip:port."""
    if len(session_id) != 8 or len(security_key) != 16:
        raise ValueError('StartGame needs an 8-byte session id and a 16-byte security key')
    writer = BitWriter()
    writer.bits(0, 1)
    writer.bits(STARTGAME_TYPE_ID, 8)
    writer.blob(common_addr(ip, port))
    writer.blob(session_id)
    writer.blob(security_key)
    return writer.out()


CHANNEL_SERVICE = 11
CREATE_CHANNEL_TASK = 13


def build_channel_record(channel_id, members=1):
    """bdChatChannelInfo for the 11/13 (createChannel) result, reader sub_A55A40:
    u64 channel id, u32 members, u32, u64 channel id, bool, bool, u32.

    The room keeps its chat-channel slot inactive until this task succeeds with
    exactly one result (completion sub_6CA0A0 sets the slot's joined flag), and
    the room-message router sub_6C9DC0 only delivers pushes whose channel id
    matches a joined slot -- so without it no room message, StartGame included,
    ever reaches the room.
    """
    return (w_u64(channel_id) + w_u32(members) + w_u32(0) + w_u64(channel_id)
            + bytes([1, 0]) + bytes([1, 0]) + w_u32(0))


SEND_MESSAGE_TASK = 4
MANIFOLD_TYPE_ID = 1
START_COUNTING_DOWN_TYPE_ID = 17


class BitReader:
    """LSB-first reader matching bdBitBuffer (the inverse of BitWriter)."""

    def __init__(self, data):
        self.value = int.from_bytes(data, 'little')
        self.size = 8 * len(data)
        self.pos = 0

    def bits(self, count):
        if self.pos + count > self.size:
            raise ValueError('room message truncated')
        value = (self.value >> self.pos) & ((1 << count) - 1)
        self.pos += count
        return value


def parse_room_messages(args):
    """11/4 (send to chat channel) args -> (channel id, [(typeId, message bytes)]).

    Args: tagged u64 channel id, blob holding [u32 length][message], tagged u32. A message is a bit
    buffer: type-check flag (0 here, so no tags follow), 8-bit typeId. dwRoomMessageManifold (typeId 1,
    writer sub_6DCE50 / reader sub_6DD020) carries u32 count, then per message a has-target bit (+ u64
    target) and a u32 byte length with the bytes; its sub-messages are returned instead of it.
    """
    if len(args) < 15 or args[0] != TAG_UINT64 or args[9] != TAG_BLOB or args[10] != TAG_UINT32:
        raise ValueError('not a channel message')
    channel = struct.unpack_from('<Q', args, 1)[0]
    size = struct.unpack_from('<I', args, 11)[0]
    blob = args[15:15 + size]
    if len(blob) != size or size < 4:
        raise ValueError('channel message blob truncated')
    length = struct.unpack_from('<I', blob)[0]
    message = blob[4:4 + length]
    reader = BitReader(message)
    reader.bits(1)
    type_id = reader.bits(8)
    if type_id != MANIFOLD_TYPE_ID:
        return channel, [(type_id, message)]
    parts = []
    for _ in range(min(reader.bits(32), 64)):
        if reader.bits(1):
            reader.bits(64)
        sub = bytes(reader.bits(8) for _ in range(reader.bits(32)))
        parts.append((BitReader(sub).bits(9) >> 1, sub))
    return channel, parts


def start_countdown(args):
    """Seconds of a dwRoomMessageStartCountingDown (typeId 17: float after the type) in an 11/4 message, or None
    if it carries none. The host sends dw_startGameCountingDown (sub_6E4CD0; 0 when it starts at once) and -1
    when a countdown is called off (sub_6E4D30: a member withdrew Ready); a negative result is that cancel.
    A truncated or NaN value reads as 0, and anything above 300 s is capped."""
    try:
        _, parts = parse_room_messages(args)
    except ValueError:
        return None
    for type_id, message in parts:
        if type_id == START_COUNTING_DOWN_TYPE_ID:
            reader = BitReader(message)
            reader.bits(9)
            try:
                seconds = struct.unpack('<f', struct.pack('<I', reader.bits(32)))[0]
            except ValueError:
                seconds = 0.0
            if seconds != seconds:
                return 0.0
            return -1.0 if seconds < 0.0 else min(seconds, 300.0)
    return None


def schedule_start(start_at, seconds, now):
    """-> the room's next start time after a StartCountingDown of `seconds` (start_countdown's result) at `now`:
    a cancel clears it, a new countdown replaces any earlier one, no countdown leaves it as it was."""
    if seconds is None:
        return start_at
    if seconds < 0.0:
        return None
    return now + seconds


def build_room_push(message, *, sender_id, sender_name, room_id, trailer=0):
    """-> (inner type, application bytes) for a type-7 room message push.

    The channel's handler (sub_6EADB0) re-reads the pushed blob as an untagged
    bdByteBuffer holding [u32 length][message] (optionally a second text blob), so
    the message is length-prefixed inside the blob.
    """
    name = bytes([TAG_STRING]) + sender_name.encode('utf-8')[:63] + b'\x00'
    payload = struct.pack('<I', len(message)) + message
    application = (w_u32(ROOM_PUSH_MESSAGE) + w_u64(sender_id) + name + w_u64(room_id)
                   + w_blob(payload) + w_u32(trailer))
    return ROOM_PUSH, application
