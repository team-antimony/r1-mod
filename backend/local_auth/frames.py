"""Authenticated QA protocol-210 data frames; direction is explicit."""
import hmac
import struct
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes


def decode_client_data(frame, keys, *, expected_counter):
    if len(frame) < 50 or frame[4:6] != b'\xab\x85' or (len(frame) - 34) % 16:
        raise ValueError('Malformed encrypted frame')
    if struct.unpack_from('<I', frame)[0] != len(frame) - 4:
        raise ValueError('Invalid frame length prefix')
    counter = struct.unpack_from('<I', frame, 6)[0]
    if counter != expected_counter:
        raise ValueError('Lobby counter mismatch')
    expected_mac = hmac.digest(keys.c2s_mac, frame[:-8], 'sha1')[:8]
    if not hmac.compare_digest(frame[-8:], expected_mac):
        raise ValueError('Lobby MAC mismatch')
    context = Cipher(algorithms.AES(keys.c2s_aes), modes.CBC(frame[10:26])).decryptor()
    plaintext = context.update(frame[26:-8]) + context.finalize()
    size = struct.unpack_from('<I', plaintext)[0]
    if size > len(plaintext) - 5:
        raise ValueError('Application length exceeds decrypted data')
    if len(plaintext) - 5 - size > 15 or any(plaintext[5 + size:]):
        raise ValueError('Invalid zero padding')
    return plaintext[4], plaintext[5:5 + size]


def encode_server_data(inner_type, application, keys, *, send_counter):
    plaintext = struct.pack('<I', len(application)) + bytes([inner_type]) + application
    plaintext += bytes((-len(plaintext)) % 16)
    context = Cipher(algorithms.AES(keys.s2c_aes), modes.CBC(bytes(16))).encryptor()
    encrypted = context.update(plaintext) + context.finalize()
    prefix = struct.pack('<I', 30 + len(encrypted)) + b'\xab\x85' + struct.pack('<I', send_counter) + bytes(16) + encrypted
    return prefix + hmac.digest(keys.s2c_mac, prefix, 'sha1')[:8]


TASK_REPLY = 0x01

# bdBitBuffer/byte_buffer type tags used by the lobby wire (matches 1.1.13 dw_wire).
TAG_UINT8, TAG_UINT32, TAG_UINT64, TAG_BLOB = 3, 8, 10, 19


def w_u8(value):
    return bytes([TAG_UINT8, value & 0xFF])


def w_u32(value):
    return bytes([TAG_UINT32]) + struct.pack('<I', value)


def w_u64(value):
    return bytes([TAG_UINT64]) + struct.pack('<Q', value)


TAG_STRING = 16


def w_str(value):
    return bytes([TAG_STRING]) + value.encode('utf-8') + b'\x00'


def build_register_codoid_result(player):
    """QA register-codoID answer: extra_data JSON carrying the local codoID."""
    import base64 as b64
    import json
    import secrets as _secrets
    codoid = struct.pack('<Q', player.user_id) + _secrets.token_bytes(8)
    nested = {'username': player.name,
              'encrypted_codoid': b64.b64encode(codoid).decode('ascii'),
              'encrypted_register_codoid_data': b64.b64encode(b'').decode('ascii')}
    document = {'extra_data': json.dumps(nested, ensure_ascii=True)}
    return json.dumps(document, ensure_ascii=True, separators=(',', ':'))


def build_task_reply(task_id, transaction_id, error=0, results=()):
    """bdRemoteTask reply: tagged u64 txn, u32 error, then u8 task + u32 count on success."""
    body = w_u64(transaction_id) + w_u32(error)
    if error == 0:
        body += w_u8(task_id) + w_u32(len(results))
        if results:
            body += w_u32(len(results)) + b''.join(results)
    return TASK_REPLY, body


def parse_task(application):
    """Split a service request into (service, task, args). Task id is a tagged u8."""
    if len(application) < 3 or application[1] != TAG_UINT8:
        raise ValueError('Malformed service request')
    return application[0], application[2], application[3:]


TAG_BOOL = 1
_FIXED_TAGS = {TAG_BOOL: '<B', TAG_UINT8: '<B', TAG_UINT32: '<I', TAG_UINT64: '<Q'}


def w_blob(data):
    """Blob: tag 19, tagged u32 length, raw bytes (QA reader sub_A392B0)."""
    return bytes([TAG_BLOB]) + w_u32(len(data)) + bytes(data)


def parse_args(args):
    """Decode a request's tagged arguments into [(tag, value)] up to the 0 terminator.

    Strings come back as bytes without their NUL, blobs as bytes.
    """
    values, offset = [], 0
    while offset < len(args):
        tag = args[offset]
        offset += 1
        if tag == 0:
            break
        if tag in _FIXED_TAGS:
            fmt = _FIXED_TAGS[tag]
            size = struct.calcsize(fmt)
            if offset + size > len(args):
                raise ValueError(f'Truncated tag {tag}')
            values.append((tag, struct.unpack_from(fmt, args, offset)[0]))
            offset += size
        elif tag == TAG_STRING:
            end = args.find(b'\x00', offset)
            if end < 0:
                raise ValueError('Unterminated string argument')
            values.append((tag, args[offset:end]))
            offset = end + 1
        elif tag == TAG_BLOB:
            if offset + 5 > len(args) or args[offset] != TAG_UINT32:
                raise ValueError('Malformed blob argument')
            size = struct.unpack_from('<I', args, offset + 1)[0]
            offset += 5
            if offset + size > len(args):
                raise ValueError('Truncated blob argument')
            values.append((tag, args[offset:offset + size]))
            offset += size
        else:
            raise ValueError(f'Unknown argument tag {tag}')
    return values


def decode_server_data(frame, keys, *, expected_counter):
    if len(frame) < 50 or frame[4:6] != b'\xab\x85' or (len(frame) - 34) % 16:
        raise ValueError('Malformed encrypted frame')
    if struct.unpack_from('<I', frame)[0] != len(frame) - 4:
        raise ValueError('Invalid frame length prefix')
    if struct.unpack_from('<I', frame, 6)[0] != expected_counter:
        raise ValueError('Lobby counter mismatch')
    if not hmac.compare_digest(frame[-8:], hmac.digest(keys.s2c_mac, frame[:-8], 'sha1')[:8]):
        raise ValueError('Lobby MAC mismatch')
    context = Cipher(algorithms.AES(keys.s2c_aes), modes.CBC(frame[10:26])).decryptor()
    plaintext = context.update(frame[26:-8]) + context.finalize()
    size = struct.unpack_from('<I', plaintext)[0]
    if size > len(plaintext) - 5 or len(plaintext) - 5 - size > 15 or any(plaintext[5 + size:]):
        raise ValueError('Invalid zero padding')
    return plaintext[4], plaintext[5:5 + size]
