"""Message buffer with the interleaved bit/byte layout.

Byte values append at the byte cursor. A bit write that starts at a byte-aligned bit position
first reserves a fresh byte at the byte cursor, then fills it LSB-first.
"""
import struct


class MsgWriter:
    def __init__(self):
        self.data = bytearray()
        self.bit = 0

    def write_bit(self, v):
        if self.bit & 7 == 0:
            self.bit = len(self.data) * 8
            self.data.append(0)
        if v & 1:
            self.data[self.bit >> 3] |= 1 << (self.bit & 7)
        self.bit += 1

    def write_bits(self, v, n):
        if n == 32:
            self.write_u32(v)
            return
        for i in range(n):
            self.write_bit((v >> i) & 1)

    def write_u8(self, v):
        self.data.append(v & 0xFF)

    def write_i16(self, v):
        self.data += struct.pack("<H", v & 0xFFFF)

    def write_u32(self, v):
        self.data += struct.pack("<I", v & 0xFFFFFFFF)

    def write_string(self, s, maxlen=None):
        b = s.encode("latin-1") if isinstance(s, str) else bytes(s)
        if maxlen is not None:
            b = b[:maxlen - 1]
        self.data += b + b"\0"

    def write_data(self, b):
        self.data += bytes(b)

    def getvalue(self):
        return bytes(self.data)


class MsgReader:
    def __init__(self, data, offset=0):
        self.data = bytes(data)
        self.rc = offset
        self.bit = 0
        self.overflow = False

    def read_u8(self):
        if self.rc >= len(self.data):
            self.overflow = True
            return 0
        v = self.data[self.rc]
        self.rc += 1
        return v

    def read_raw(self, n):
        if self.rc + n > len(self.data):
            self.overflow = True
            self.rc = len(self.data)
            return b"\0" * n
        v = self.data[self.rc:self.rc + n]
        self.rc += n
        return v

    def read_i16(self):
        return struct.unpack("<h", self.read_raw(2))[0]

    def read_u16(self):
        return struct.unpack("<H", self.read_raw(2))[0]

    def read_u32(self):
        return struct.unpack("<I", self.read_raw(4))[0]

    def read_string(self, maxlen=1 << 16):
        out = bytearray()
        while True:
            if self.rc >= len(self.data):
                self.overflow = True
                break
            c = self.data[self.rc]
            self.rc += 1
            if c == 0:
                break
            if len(out) < maxlen - 1:
                out.append(c)
        return bytes(out)

    def read_bit(self):
        if self.bit & 7 == 0:
            if self.rc >= len(self.data):
                self.overflow = True
                return 0
            self.bit = self.rc * 8
            self.rc += 1
        v = (self.data[self.bit >> 3] >> (self.bit & 7)) & 1
        self.bit += 1
        return v

    def read_bits(self, n):
        if n == 32:
            return self.read_u32()
        v = 0
        for i in range(n):
            v |= self.read_bit() << i
        return v

    def remaining(self):
        return len(self.data) - self.rc
