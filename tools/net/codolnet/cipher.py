"""Payload cipher and usercmd key hash."""


def key_text(command):
    """The part of a stored command the cipher walks: up to the first NUL."""
    return bytes(command).split(b"\0", 1)[0]


def xor_payload(payload, key_byte, text):
    """Rolling XOR used in both directions. Symmetric: the same call encrypts and decrypts."""
    text = key_text(text)
    out = bytearray(payload)
    key = key_byte & 0xFF
    j = 0
    for i in range(len(out)):
        if j >= len(text):
            j = 0
        c = text[j] if text else 0
        j += 1
        key ^= (c << (i & 1)) & 0xFF
        out[i] ^= key
    return bytes(out)


def _to_i32(v):
    v &= 0xFFFFFFFF
    return v - (1 << 32) if v & 0x80000000 else v


def com_hash_key(text):
    """Signed chars, at most 32, weights i + 119; then h ^ ((h ^ (h >> 10)) >> 10)."""
    h = 0
    for i, c in enumerate(key_text(text)[:32]):
        c = c - 256 if c >= 128 else c
        h = _to_i32(h + c * (i + 119))
    return _to_i32(h ^ ((h ^ (h >> 10)) >> 10))
