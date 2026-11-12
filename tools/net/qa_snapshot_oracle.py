"""Snapshot (svc 0) decoder for CODOL, following the QA 3.3.3.3 client's reader, with field tables loaded from a JSON dump
(qa_netfields_aligned.json next to this file).

Values are tracked as raw 32-bit words per struct offset so deltas resolve against the right
baseline. The parse is self-checking: after the glass section the next byte of the message
must be svc 6 (EOF).
"""
import json
import math
import struct

ANGLE_STEPS = [364, 546, 182, 1, 910, 1092, 729, 257]   # word_CDCB3C
# Per-type parameter widths for codes -94/-93 (QA dword_C56D70; same table in 2.6.9.7 @0xCABCE0).
TYPE_BITS = [11, 0, 6, 14, 14, 32, 32, 32, 32, 0, 14, 6, 2, 2, 2, 2, 10, 10, 0, 0, 0, 0, 0, 0,
             0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 10, 10, 5, 5, 8, 8, 0, 0, 5, 5, 0, 5, 5, 5, 5, 5, 5, 5,
             5, 0, 0, 10, 10, 10, 2, 2, 0, 11, 0, 5, 5, 11, 8, 8, 8, 0, 0, 0, 8, 0, 8, 8, 8, 8, 8,
             8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 9, 8, 17, 17, 17, 17, 9, 10, 8, 0, 0, 0, 0, 0,
             0, 10, 0, 0, 0, 10, 19, 10, 0, 0, 0, 0, 0, 8, 0, 0, 6, 6, 6, 6, 6, 10, 13, 0, 0, 5, 5,
             11, 11]
ANGLE_UNIT = 360.0 / 65536.0
U32 = 0xFFFFFFFF


class ParseError(Exception):
    pass


def f2u(f):
    return struct.unpack("<I", struct.pack("<f", f))[0]


def u2f(u):
    return struct.unpack("<f", struct.pack("<I", u & U32))[0]


def c_int(f):
    """C (int) cast of a float: truncate toward zero."""
    if math.isnan(f) or math.isinf(f):
        return 0
    return int(f)


def bitlen_for(n):
    """32 - (bsr(n) ^ 31) == bsr(n) + 1"""
    return n.bit_length()


def type_bits(i):
    return TYPE_BITS[i] if 0 <= i < len(TYPE_BITS) else 0


class Profile:
    def __init__(self, tables, ps_tail="qa", etype_clamp=None, ammo_bits=(10, 9), clip_bits=(10, 11),
                 anim_format="qa", ent_lc_bits=None, trailing_gates=1):
        self.trailing_gates = trailing_gates
        self.anim_format = anim_format
        self.ent_lc_bits = ent_lc_bits
        self.t = tables
        self.ps_fields = tables["playerState"]
        self.clients = tables["clients"] if "clients" in tables else tables["clients76"]
        self.new24 = tables["newClient24"]
        self.objective = tables["objective"]
        self.hud = tables["hudelem"]
        self.ent_slots = [tables["entity_" + s[0]] for s in tables["entity_slots"]]
        self.etype_clamp = etype_clamp if etype_clamp is not None else len(self.ent_slots) - 1
        self.ps_tail = ps_tail
        self.ammo_bits, self.clip_bits = ammo_bits, clip_bits


class Snapshots:
    def __init__(self, profile, map_center=(0.0, 0.0, 0.0)):
        self.p = profile
        self.center = map_center
        self.frames = {}          # serverMessageSequence -> frame dict
        self.debug = False

    def mark(self, m, what):
        if self.debug:
            print(f"    [{what}] byte {m.rc} bitpos {m.bit}")

    # ------------------------------------------------------------------ fields
    def read_field(self, m, field, to, frm, time, no_xor_from=False, idx=0):
        off, bits, _hint = field
        fv = 0 if no_xor_from else frm.get(off, 0)
        ff = u2f(fv)

        def put(v):
            to[off] = v & U32

        def put_f(x):
            to[off] = f2u(x)

        def xor_bits(n):
            neg = n < 0
            n = abs(n)
            v = m.bits(n & 7) if n & 7 else 0
            shift = n & 7
            while shift < n:
                v |= m.byte() << shift
                shift += 8
            mask = U32 if n == 32 else (1 << n) - 1
            r = (fv & mask) ^ v
            if neg and r & (1 << (n - 1)):
                r |= ~mask & U32
            return r

        def float13(base_bias=4096, low_bits=5):
            v = m.bits(low_bits) + (m.byte() << low_bits)
            return float(((v ^ (c_int(ff) + base_bias)) - base_bias))

        def angle():
            v9 = ff * 182.04444885253906 + 0.5
            if m.readbit() == 1:
                sign = -1 if m.readbit() else 1
                step = ANGLE_STEPS[m.bits(3)]
                s16 = ((c_int(v9 - 0.4999999990686774) + sign * step) + 0x8000) % 0x10000 - 0x8000
                return s16 * ANGLE_UNIT
            if m.readbit():
                sign = -1 if m.readbit() else 1
                s16 = ((c_int(v9 - 0.4999999990686774) + sign * m.bits(12)) + 0x8000) % 0x10000 - 0x8000
                return s16 * ANGLE_UNIT
            return m.short() * ANGLE_UNIT

        def origin(axis):
            if m.readbit():
                c = c_int(self.center[axis] + 9.313225746154785e-10)
                return float((m.bits(16) ^ ((c_int(ff + 9.313225746154785e-10) - c + 0x8000) & 0xFFFF)) + c - 0x8000)
            return float(m.bits(7) - 64) + ff

        if bits == 0 or bits in (-64, -63, -62):
            if not m.readbit():
                put(m.readbit() << 31)
            elif not m.readbit():
                put_f(float13())
            else:
                put(m.long() ^ fv)
        elif bits == -89:
            if not m.readbit():
                put_f(float13())
            else:
                put(m.long() ^ fv)
        elif bits in (-88, -61, -60, -59):
            put(m.long() ^ fv)
        elif bits == -99:
            if m.readbit():
                if m.readbit():
                    put(m.long() ^ fv)
                else:
                    put_f(float13(2048, 4))
            else:
                put(0)
        elif bits in (-100, -79):
            put_f(angle())
        elif bits == -98:
            put(m.bits(30) if m.readbit() == 1 else fv ^ (1 << m.bits(5)))
        elif bits in (-97, -74, -72, -70):
            put(time - m.bits(8) if m.readbit() == 0 else m.long())
        elif bits == -96:
            if m.readbit() == 1:
                put(2046)
            elif m.readbit() == 1:
                put(0)
            else:
                lo = m.bits(3)
                put(lo | (m.byte() << 3))
        elif bits == -94:
            if m.readbit() == 0:
                t = frm.get(off, 0) if not no_xor_from else 0
                to[off] = t
                n = type_bits(t)
                to[off + 4] = m.bits(n) if n else 0
            else:
                t = m.byte()
                to[off] = t
                n = type_bits(t)
                if n == 0:
                    to[off + 4] = 0
                elif m.readbit():
                    to[off + 4] = m.bits(n)
                else:
                    to[off + 4] = frm.get(off + 4, 0) & ((1 << n) - 1 if n < 32 else U32)
        elif bits == -93:
            n = type_bits(idx)
            put(m.bits(n) if n else 0)
        elif bits in (-92, -83):
            put_f(origin(0))
        elif bits in (-91, -82):
            put_f(origin(1))
        elif bits in (-90, -81):
            put_f(origin(2))
        elif bits == -87:
            put_f(m.short() * ANGLE_UNIT)
        elif bits == -86:
            put_f(m.bits(6) / 10.0)
        elif bits == -85:
            if m.readbit():
                rgb = fv & 0xFFFFFF
                a = 0xFF if ((fv >> 24) & 0xFF) == 0 else 0
                put(rgb | (a << 24))
            else:
                if m.readbit():
                    rgb = fv & 0xFFFFFF
                elif m.readbit():
                    r, g, b = m.byte(), m.byte(), m.byte()
                    rgb = r | (g << 8) | (b << 16)
                else:
                    m.bits(3)          # palette index (table not modelled)
                    rgb = 0
                a5 = m.bits(5)
                put(rgb | ((0xFF if a5 == 31 else (a5 * 8) & 0xFF) << 24))
        elif bits == -84:
            put(m.bits(7))
        elif bits == -77:
            put(m.byte())
        elif bits == -76:
            put((fv + m.bits(5) - 16) if m.readbit() == 1 else xor_bits(-8))
        elif bits == -75:
            put((fv + m.bits(3) + 1) if m.readbit() == 1 else xor_bits(8))
        elif bits in (-80, -78, -73, -71):
            put(m.long())
        elif bits == -69:
            if not m.readbit():
                put(time)
            elif not m.readbit():
                put(time - 50 * m.bits(3))
            elif m.readbit():
                put(m.long())
            else:
                put(time - 50 * m.bits(8))
        elif bits == -65:
            put(100 * m.bits(10))
        elif bits == -58:
            if m.readbit():
                put(m.long())
            else:
                put_f(m.bits(2) * (20.0 / 65536.0) - 10.0)
        elif bits == -57:
            put(m.readbit())
        elif bits == -56:
            lo, hi = fv & 0xFFFF, fv >> 16
            if m.readbit():
                lo ^= m.bits(10)
            if m.readbit():
                hi ^= m.bits(10)
            put(lo | (hi << 16))
        elif bits == -55:
            v = fv
            if m.readbit() == 1:
                v = (v & ~0x7F) | m.bits(4) | (m.bits(3) << 4)
            if m.readbit() == 1:
                v = (v & ~0x1F80) | (m.readbit() << 12) | (m.bits(5) << 7)
            put(v)
        elif bits == -101:
            if m.readbit():
                put(m.bits(6))
        elif bits < -55 and bits not in (-16, -8):
            raise ParseError(f"unhandled field code {bits}")
        else:   # positive widths and small negative (signed) widths
            put(xor_bits(bits) if m.readbit() else 0)

    def gap(self, m, mode):
        if mode == 1:
            n = 0
            while True:
                n += 1
                if m.readbit():
                    return n
                if m.overflow:
                    return n
        if m.readbit():
            return 1
        total = 1
        top = (1 << mode) - 1
        v = m.bits(mode)
        while v == top and not m.overflow:
            total += top
            v = m.bits(mode)
        return total + v

    def delta_struct(self, m, table, frm, time, mode):
        to = dict(frm)
        if not m.readbit():
            return to
        last = m.bits(bitlen_for(len(table)))
        if last > len(table):
            raise ParseError(f"struct lastChanged {last} > {len(table)}")
        i = -1
        while True:
            i += self.gap(m, mode)
            if i < 0 or i >= len(table):
                raise ParseError(f"struct field index {i} out of range")
            self.read_field(m, table[i], to, frm, time)
            if i == last:
                break
        for k in range(last):
            if table[k][2] == 2:
                self.read_field(m, table[k], to, frm, time)
        return to

    # ------------------------------------------------------------ playerstate
    def playerstate(self, m, frm, time):
        p = self.p
        table = p.ps_fields
        to = dict(frm)
        predicted = m.readbit() > 0
        count = m.bits(bitlen_for(len(table) + 1))
        if count > len(table) + 1:
            raise ParseError(f"ps lastChanged {count} > {len(table) + 1}")
        i = -1
        if count > 0:
            while True:
                i += self.gap(m, 4)
                if i >= len(table):
                    raise ParseError(f"ps field index {i}")
                f = table[i]
                self.read_field(m, f, to, frm, time, no_xor_from=predicted and f[2] == 3)
                if i >= count - 1:
                    break
            for k in range(count - 1):
                if table[k][2] == 2:
                    self.read_field(m, table[k], to, frm, time)
        self.mark(m, f"ps fields done (predicted={predicted} count={count} last={i})")
        # stats
        if m.readbit():
            mask = m.bits(4)
            for b in range(3):
                if mask & (1 << b):
                    m.short()
            if mask & 8:
                m.byte()
        self.mark(m, "stats done")
        if p.ps_tail in ("qa", "qa-noblocks", "c1"):
            self.ps_tail_qa(m, blocks=p.ps_tail == "qa")
        else:
            self.ps_tail_iw4(m)
        self.mark(m, "ammo/weapons/clips done")
        # objectives
        if m.readbit():
            objs = frm.get("_obj", [{} for _ in range(32)])
            new = []
            for k in range(32):
                m.bits(3)
                new.append(self.delta_struct(m, p.objective, objs[k], time, 1))
            to["_obj"] = new
        self.mark(m, "objectives done")
        # hud elems
        if m.readbit():
            for which in ("_hudA", "_hudC"):
                to[which] = self.hud(m, frm.get(which, []), time)
                self.mark(m, f"hud {which} done ({len(to[which])} elems)")
        if p.ps_tail in ("qa", "qa-noblocks"):
            self.ps_lists_qa(m)
        elif p.ps_tail == "c1":
            # Trailing gated sections after the hud elems (reference server: one, always 0).
            for k in range(p.trailing_gates):
                if m.readbit():
                    raise ParseError(f"ps trailing section {k} present (layout unknown)")
        return to

    def ps_tail_qa(self, m, blocks=True):
        abits, cbits = self.p.ammo_bits, self.p.clip_bits
        dbg = []
        if m.readbit():                                  # ammo list
            while True:
                idx = m.bits(4)
                w = m.bits(abits[0])
                counts = [m.bits(abits[1]) if m.readbit() else None for _ in range(2)]
                dbg.append(("ammo", idx, w, counts))
                if idx > 14:
                    raise ParseError("ammo index > 14")
                if not m.readbit() or m.overflow:
                    break
        if m.readbit():                                  # weapon state x15
            for k in range(15):
                if m.readbit():
                    w = m.bits(10) if m.readbit() else None
                    flags = m.bits(5)
                    dbg.append(("weap", k, w, flags))
        if m.readbit():                                  # clip list
            while True:
                idx = m.bits(4)
                a, b = m.bits(cbits[0]), m.bits(cbits[1])
                dbg.append(("clip", idx, a, b))
                if idx > 14:
                    raise ParseError("clip index > 14")
                if not m.readbit() or m.overflow:
                    break
        if self.debug:
            for d in dbg:
                print("      ", d)
        if not blocks:
            return
        if m.readbit():                                  # block A (asym powers) x3
            for _ in range(3):
                if not m.readbit():                      # 1 = slot changed
                    continue
                if m.readbit():
                    m.bits(4)
                if m.readbit():
                    m.bits(3)
                if m.readbit():
                    m.short()
                if m.readbit():
                    m.bits(2)
                if m.readbit():
                    m.bits(28)
                if m.readbit():
                    m.bits(3)
        if m.readbit():                                  # block B (zarena) x7
            for _ in range(7):
                if not m.readbit():                      # 1 = entry changed
                    continue
                if m.readbit():
                    m.short()
                if m.readbit():
                    m.bits(28)
        if m.readbit():                                  # block C
            if m.readbit():
                m.bits(12)
            if m.readbit():
                m.short()
            if m.readbit():
                m.bits(3)
            if m.readbit():
                m.bits(2)
            if m.readbit():
                m.bits(2)
            if m.readbit():
                m.bits(11)
        if m.readbit():                                  # block D
            if m.readbit():
                m.bits(2)
            if m.readbit():
                m.long()
            if m.readbit():
                m.bits(6)

    def ps_tail_iw4(self, m):
        abits, cbits = self.p.ammo_bits, self.p.clip_bits
        while m.readbit():                               # ammo list
            m.bits(4)
            m.bits(abits[0])
            for _ in range(2):
                if m.readbit():
                    m.bits(abits[1])
            if m.overflow:
                break
        if m.readbit():                                  # weapon state x15
            for _ in range(15):
                if m.readbit():
                    if m.readbit():
                        m.bits(abits[0])
                    for _ in range(4):
                        m.readbit()
                    if m.readbit():
                        m.byte()
        while m.readbit():                               # clip list
            m.bits(4)
            m.bits(cbits[0])
            m.bits(cbits[1])
            if m.overflow:
                break

    def ps_lists_qa(self, m):
        for idx_bits, fields in ((5, (16,)), (5, (4, 3, 12, 11)), (3, (4, 4, 12)), (4, (8,))):
            if m.readbit():
                while True:
                    m.bits(idx_bits)
                    for b in fields:
                        m.bits(b)
                    if not m.readbit() or m.overflow:
                        break

    def hud(self, m, frm_list, time):
        n = m.bits(5)
        if n >= 32:
            raise ParseError(f"hud count {n}")
        out = []
        for k in range(n):
            frm = frm_list[k] if k < len(frm_list) else {}
            to = dict(frm)
            last = m.bits(6)
            if last:
                i = -1
                while True:
                    i += self.gap(m, 3)
                    if i > 41:
                        raise ParseError(f"hud field {i}")
                    self.read_field(m, self.p.hud[i], to, frm, time)
                    if i >= last - 1:
                        break
            out.append(to)
        return out

    # ---------------------------------------------------------------- sections
    def anim_cmds_c1(self, m, prev_count):
        """1.1.1.13-family format (as written by the reference server): per record a 0 bit
        (1 = reuse previous record), then 4-bit opcodes; opcode 0 ends the section."""
        n = 0
        prev_bits = 0 if prev_count == 0 else max(1, (prev_count - 1).bit_length())
        while not m.overflow:
            if m.readbit():
                m.bits(prev_bits)
                n += 1
                continue
            while True:
                op = m.bits(4)
                if op == 0:
                    return n
                if op == 7:
                    m.bits(11)
                    continue
                if op == 8:
                    if m.readbit():
                        m.long()
                    else:
                        m.bits(8)
                    continue
                if not 1 <= op <= 6:
                    raise ParseError(f"anim opcode {op}")
                if 3 <= op <= 5:
                    m.bits(2)
                m.bits(11)
                if op == 5:
                    if m.readbit():
                        m.bits(11)
                    else:
                        m.readbit()
                if op <= 2:
                    self._c1_blend(m)
                elif op <= 5:
                    if m.readbit():
                        m.readbit()
                        m.bits(5)
                    else:
                        m.readbit()
                    self._c1_blend(m)
                    if m.readbit():
                        m.readbit()
                        m.bits(5)
                else:
                    m.bits(5)
                n += 1
                break
        return n

    @staticmethod
    def _c1_blend(m):
        if m.readbit() and m.readbit():
            m.bits(4)

    def anim_cmds(self, m, prev_count):
        if self.p.anim_format == "c1":
            return self.anim_cmds_c1(m, prev_count)
        if not m.readbit():
            return 0
        dict_bits = 0
        if m.readbit():
            count = m.byte()
            for _ in range(count):
                m.bits(11)
            dict_bits = 0 if count <= 1 else (count - 1).bit_length()
        prev_bits = 0 if prev_count == 0 else max(1, (prev_count - 1).bit_length())
        n = 0
        while not m.overflow:
            if m.readbit():
                m.bits(prev_bits)
            elif self.anim_cmd(m, dict_bits):
                break
            n += 1
        return n

    def anim_cmd(self, m, dict_bits):
        def weight():
            if m.readbit():
                m.readbit()
                m.bits(5)
            else:
                m.readbit()

        def rate():
            if m.readbit():
                m.readbit()
                m.bits(5)

        def blend():
            if m.readbit() and m.readbit():
                if m.readbit():
                    m.bits(2)
                else:
                    m.bits(3)

        op = 3
        while op in (3, 4):
            if m.readbit():
                op = m.bits(2) + 1
            elif m.readbit():
                op = m.bits(2) + 5
            elif m.readbit():
                op = m.bits(2) + 9
            else:
                op = m.bits(2) + 13
            if op == 3:
                m.bits(11)
            elif op == 4:
                if m.readbit() == 0:
                    m.bits(8)
                else:
                    m.long()
            elif op in (1, 2, 8):
                m.bits(2)
                m.bits(dict_bits)
                if op == 8:
                    m.bits(dict_bits)
                weight()
                blend()
                rate()
            elif op == 6:
                weight()
            elif op in (7, 9):
                blend()
            elif op == 10:
                pass
            elif op in (11, 12):
                m.bits(dict_bits)
                blend()
            elif op == 13:
                m.bits(dict_bits)
                m.bits(5)
            else:
                return True
        return False

    def indexed_list(self, m, old, table, time, mode, template=None):
        """clients / new24 sections: bit continue, index (bit=+1 / 5 bits), bit removed, delta."""
        out = dict(old)
        m.last_ref = -1
        while not m.overflow:
            if not m.readbit():
                break
            m.last_ref = m.last_ref + 1 if m.readbit() else m.bits(5)
            idx = m.last_ref
            if m.readbit() == 1:
                out.pop(idx, None)
                continue
            frm = old.get(idx, template or {})
            out[idx] = self.delta_struct(m, table, frm, time, mode)
        return out

    def entities(self, m, old, time):
        out = dict(old)
        ref = -1
        p = self.p
        while not m.overflow:
            if m.readbit():
                ref += 1
            elif m.readbit():
                ref = m.bits(11)
            else:
                ref += m.bits(9)
            if ref == 2047:
                break
            if ref > 2047:
                raise ParseError(f"entity number {ref}")
            frm = old.get(ref, {})
            if m.readbit() == 1:                  # removed
                out.pop(ref, None)
                continue
            if m.readbit() == 0:                  # unchanged
                out[ref] = dict(frm)
                continue
            flag = m.readbit() != 0
            nfields = len(p.ent_slots[0])
            last = m.bits(p.ent_lc_bits or bitlen_for(nfields - 1))
            if last > nfields - 1:
                raise ParseError(f"entity {ref} lastChanged {last}")
            to = dict(frm)
            general = p.ent_slots[0]
            i = self.gap(m, 3) - 1
            if i <= 0:
                self.read_field(m, general[0], to, frm, time)
                cur = 0
                if last != 0:
                    i = self.gap(m, 3)
            else:
                cur = -1
            etype = to.get(general[0][0], 0)
            slot = min(etype, p.etype_clamp)
            table = p.ent_slots[slot]
            sub = max(etype - p.etype_clamp, 0)
            changed_type = frm.get(general[0][0], 0) != etype
            if not (cur == 0 and last == 0):
                while True:
                    if i > nfields - 1:
                        raise ParseError(f"entity {ref} field {i}")
                    f = table[i]
                    nox = changed_type or (flag and -83 <= f[1] <= -78)
                    self.read_field(m, f, to, frm, time, no_xor_from=nox, idx=sub)
                    if i == last:
                        break
                    i += self.gap(m, 3)
                    if i > last:
                        break
            for k in range(last):
                f = table[k]
                if f[2] == 2:
                    nox = changed_type or (flag and -83 <= f[1] <= -78)
                    self.read_field(m, f, to, frm, time, no_xor_from=nox, idx=sub)
            out[ref] = to
        return out

    def glass(self, m):
        if not m.readbit():
            return
        if m.readbit():
            while not m.overflow:
                i = m.bits(11)
                if i == 1024:
                    break
                if m.bits(2) == 2 and m.readbit():
                    m.byte()
                    m.bits(6)
                    m.bits(6)
        else:
            for _ in range(m.bits(11)):
                if m.overflow:
                    break
                if m.bits(2) == 2 and m.readbit():
                    m.byte()
                    m.bits(6)
                    m.bits(6)

    # -------------------------------------------------------------- snapshot
    def snapshot(self, m, sms):
        server_time = m.long()
        delta_num = m.byte()
        flags = m.byte()
        old = self.frames.get(sms - delta_num) if delta_num else None
        if delta_num and old is None:
            raise ParseError(f"delta frame {sms - delta_num} unknown")
        old = old or {"ps": {}, "anim": 0, "new24": {}, "ents": {}, "clients": {}}
        ps_from = old["ps"] if m.readbit() > 0 else {}
        ps = self.playerstate(m, ps_from, server_time)
        anim = self.anim_cmds(m, old["anim"])
        new24 = self.indexed_list(m, old["new24"], self.p.new24, server_time, 3)
        ents = self.entities(m, old["ents"], server_time)
        clients = self.indexed_list(m, old["clients"], self.p.clients, server_time, 1)
        self.glass(m)
        if m.overflow:
            raise ParseError("message overrun inside snapshot")
        self.frames[sms] = {"ps": ps, "anim": anim, "new24": new24, "ents": ents,
                            "clients": clients, "time": server_time}
        for k in [k for k in self.frames if k < sms - 64]:
            del self.frames[k]
        return {"time": server_time, "delta": delta_num, "flags": flags,
                "entities": len(ents), "clients": len(clients), "anim": anim}


def load_profile(path, **kw):
    return Profile(json.load(open(path)), **kw)
