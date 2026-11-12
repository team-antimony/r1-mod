"""QA 3.3.3.3 bdDTLS DATA records (the game-server UDP transport).

Pinned against live traffic (tests/test_dtls.py): the client's first record
decrypts to the connectionless packet `ff ff ff ff getchallenge 0 "<id>"`.
This is NOT the scheme in the 1.1.13 sh1x server (Tiger-192 / AES-192), which
rejects every QA record.

Record (little endian):  06 02 | u16 destination tag | u16 counter (low 16 bits)
                         | 8-byte MAC | u16 payload length | ciphertext

  key     = SHA1(ECDH x-coordinate (28 B) || 16-byte security key)      (20 bytes)
  cipher  = AES-128-CTR, key = key[:16], counter block = SHA1(u32 counter ||
            u16 tag)[:16] incremented little endian per 16-byte block, where
            counter is the full 32-bit record counter and tag the destination
            tag the record carries
  mac     = HMAC-SHA1(key, header[0:6] || length || ciphertext)[:8]

The handshake before it (NAT-trav INTRO, INIT, COOKIE) is unchanged from
1.1.13 and already handled by sh1x.
"""
import hashlib
import hmac
import struct

from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

RECORD_TYPE = b'\x06\x02'
HEADER = 16


def derive_key(shared_x, security_key):
    if len(shared_x) != 28 or len(security_key) != 16:
        raise ValueError('need a 28-byte ECDH x and a 16-byte security key')
    return hashlib.sha1(shared_x + security_key).digest()


def record_mac(key, header6, length, ciphertext):
    return hmac.new(key, header6 + length + ciphertext, hashlib.sha1).digest()[:8]


def _crypt(data, key, counter, tag):
    block = bytearray(hashlib.sha1(struct.pack('<IH', counter, tag)).digest()[:16])
    ecb = Cipher(algorithms.AES(key[:16]), modes.ECB()).encryptor()
    stream = bytearray()
    for _ in range(0, len(data), 16):
        stream += ecb.update(bytes(block))
        for i in range(16):                      # little-endian increment
            block[i] = (block[i] + 1) & 0xFF
            if block[i]:
                break
    return bytes(a ^ b for a, b in zip(data, stream))


def extend_counter(low, last):
    """Widen a 16-bit wire counter to 32 bits, nearest to the last full counter."""
    base = last & 0xFFFF0000
    best = base | low
    for candidate in (best - 0x10000, best, best + 0x10000):
        if candidate >= 0 and abs(candidate - last) < abs(best - last):
            best = candidate
    return best


def encrypt_record(plain, key, counter, tag):
    if len(plain) > 0xFFFF:
        raise ValueError('record too large')
    header = RECORD_TYPE + struct.pack('<HH', tag, counter & 0xFFFF)
    cipher = _crypt(plain, key, counter, tag)
    length = struct.pack('<H', len(plain))
    return header + record_mac(key, header, length, cipher) + length + cipher


def decrypt_record(record, key, expected_tag, last_counter=0):
    """-> (full counter, plaintext); ValueError for any malformed/unauthentic record."""
    if len(record) < HEADER or record[:2] != RECORD_TYPE:
        raise ValueError('not a DATA record')
    if struct.unpack_from('<H', record, 2)[0] != expected_tag:
        raise ValueError('destination tag does not match association')
    length = struct.unpack_from('<H', record, 14)[0]
    if length != len(record) - HEADER:
        raise ValueError('payload length does not match record')
    cipher = record[HEADER:]
    expected = record_mac(key, record[:6], record[14:16], cipher)
    if not hmac.compare_digest(expected, record[6:14]):
        raise ValueError('record authentication failed')
    counter = extend_counter(struct.unpack_from('<H', record, 4)[0], last_counter)
    return counter, _crypt(cipher, key, counter, expected_tag)


class QaAssociation:
    """One established bdDTLS association from this end's point of view."""

    def __init__(self, shared_x, security_key, *, local_tag, peer_tag):
        self.key = derive_key(shared_x, security_key)
        self.local_tag = local_tag
        self.peer_tag = peer_tag
        self.send_counter = 1
        self.receive_high = 0
        self._seen = set()

    def seal(self, plain):
        record = encrypt_record(plain, self.key, self.send_counter, self.peer_tag)
        self.send_counter += 1
        return record

    def open(self, record):
        counter, plain = decrypt_record(record, self.key, self.local_tag, self.receive_high)
        if counter in self._seen:
            raise ValueError('duplicate counter')
        self._seen.add(counter)
        self.receive_high = max(self.receive_high, counter)
        return plain
