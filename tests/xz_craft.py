"""Hand-edited .xz streams for tests/test_c_xz_dec.py: take a stream liblzma wrote and change one field with the CRC-32s that guard it fixed up
(or not), so the decoder's own check of that field is what is tested. liblzma is the judge of what is valid (xz_support.xz_d)."""

import lzma
import struct
import zlib

from xz_support import MAGIC, block_header, stream_header, vli, xz


def crc(b: bytes) -> bytes:
    return struct.pack("<I", zlib.crc32(b))


class Parsed:
    """Offsets inside a one-block xz stream written by liblzma."""

    def __init__(self, s: bytes) -> None:
        self.s = s
        self.hs = (s[12] + 1) * 4
        self.data = 12 + self.hs
        i = self.data
        while s[i]:
            c = s[i]
            i += 3 + s[i + 1] * 256 + s[i + 2] + 1 if c < 0x80 else 5 + (c >= 0xC0) + s[i + 3] * 256 + s[i + 4] + 1
        self.lzma2_len = i + 1 - self.data
        self.pad = -self.lzma2_len % 4
        self.check = {0: 0, 1: 4, 4: 8, 10: 32}[s[7] & 15]
        self.index = self.data + self.lzma2_len + self.pad + self.check
        self.footer = len(s) - 12


def edit_header(s: bytes, fn) -> bytes:
    p = Parsed(s)
    body = bytearray(s[12:12 + p.hs - 4])
    fn(body)
    return s[:12] + bytes(body) + crc(bytes(body)) + s[12 + p.hs:]


def edit_index(s: bytes, fn) -> bytes:
    """fn changes the index bytes (indicator .. before the CRC, padding included); the CRC-32 is recomputed, the footer left as it is."""
    p = Parsed(s)
    idx = bytearray(s[p.index:p.footer - 4])
    fn(idx)
    return s[:p.index] + bytes(idx) + crc(bytes(idx)) + s[p.footer:]


def edit_footer(s: bytes, fn) -> bytes:
    p = Parsed(s)
    f = bytearray(s[p.footer + 4:p.footer + 10])
    fn(f)
    return s[:p.footer] + crc(bytes(f)) + bytes(f) + b"YZ"


def retag(s: bytes, new_id: int) -> bytes:
    """The same stream claiming another check type of the same field size (header and footer flags and their CRCs fixed)."""
    b = bytearray(s)
    b[7] = new_id
    b[8:12] = crc(bytes(b[6:8]))
    f = len(b) - 12
    b[f + 9] = new_id
    b[f:f + 4] = crc(bytes(b[f + 4:f + 10]))
    return bytes(b)


def make_index(recs: list[tuple[int, int]], count: int | None = None, pad_byte: int = 0) -> bytes:
    body = b"\x00" + vli(len(recs) if count is None else count) + b"".join(vli(u) + vli(c) for u, c in recs)
    body += bytes([pad_byte] * (-len(body) % 4))
    return body + crc(body)


def with_index(s: bytes, index: bytes) -> bytes:
    """The stream with another index and a footer that matches it (backward size, flags)."""
    p = Parsed(s)
    flags = bytes([0, s[7] & 15])
    back = struct.pack("<I", len(index) // 4 - 1) + flags
    return s[:p.index] + index + crc(back) + back + b"YZ"


def _hdr(s: bytes, p: Parsed, csize: int | None, usize: int | None) -> bytes:
    # a header without sizes is [size][flags=0][0x21][0x01][dict code][padding...]: the dictionary code is at body[4]
    return block_header([(0x21, bytes([s[12 + 4]]))], csize, usize)


def with_sizes(s: bytes, plain_len: int, csize: int | None, usize: int | None) -> bytes:
    """A block header that states the compressed / uncompressed size, with the index and footer fixed to the longer header."""
    p = Parsed(s)
    h = _hdr(s, p, csize, usize)
    t = s[:12] + h + s[12 + p.hs:]
    return with_index(t, make_index([(len(h) + p.lzma2_len + p.check, plain_len)]))


def raw_lzma2(data: bytes, **o) -> bytes:
    """The LZMA2 chunks liblzma writes for data on its own (it starts with a dictionary reset and properties)."""
    return lzma.compress(data, format=lzma.FORMAT_RAW, filters=[{"id": lzma.FILTER_LZMA2, "preset": 1, **o}])[:-1]    # without the end mark


def no_props(chunks: bytes, ctl: int = 0xA0) -> bytes:
    """The first LZMA chunk of a raw LZMA2 stream turned into a chunk that resets only the state (the properties byte dropped)."""
    assert chunks[0] >= 0xE0
    return bytes([ctl | chunks[0] & 0x1F]) + chunks[1:5] + chunks[6:]


def wrap(lzma2: bytes, usize: int, dict_code: int = 16) -> bytes:
    """An xz stream (no check) around hand-assembled LZMA2 chunks."""
    data = lzma2 + b"\x00"
    hdr = block_header([(0x21, bytes([dict_code]))])
    pad = bytes(-len(data) % 4)
    return stream_header(0) + hdr + data + pad + with_index_only([(len(hdr) + len(data), usize)])


def with_index_only(recs: list[tuple[int, int]]) -> bytes:
    index = make_index(recs)
    back = struct.pack("<I", len(index) // 4 - 1) + bytes([0, 0])
    return index + crc(back) + back + b"YZ"


def mixed_chunks() -> list[tuple[str, bytes]]:
    a, b = bytes(range(256)) * 16, b"abcdefgh" * 500 + bytes(range(97, 123)) * 40               # len(a) is a multiple of 4: positions line up
    ra, rb = raw_lzma2(a), raw_lzma2(b)
    tiny = raw_lzma2(b"xyz" * 100, lc=0, lp=0, pb=0)
    stored = b"\x01\x00\x03abcd"
    stored_more = b"\x02\x00\x03abcd"
    return [("state reset between chunks", wrap(ra + no_props(rb), len(a) + len(b))),
            ("dictionary reset in the middle", wrap(ra + rb, len(a) + len(b))),
            ("stored chunk without dictionary reset", wrap(ra + stored_more, len(a) + 4)),
            ("stored chunk with a dictionary reset first", wrap(stored + ra, len(a) + 4)),
            ("stored dictionary reset, then a chunk without properties", wrap(stored + no_props(tiny), 4 + 300)),
            ("chunk without any reset after the first", wrap(ra + bytes([0x80 | rb[0] & 0x1F]) + rb[1:5] + rb[6:], len(a) + len(b))),
            ("first chunk stored without dictionary reset", wrap(stored_more + ra, len(a) + 4))]


def crafted(plain: bytes) -> list[tuple[str, bytes]]:
    """(label, stream): edits of a one-block stream of `plain` (CRC-64 check, and one with no check for the LZMA2 data edits)."""
    s = xz(plain, "crc64", preset=1)
    n = xz(plain, "none", preset=1)
    p, pn = Parsed(s), Parsed(n)
    out: list[tuple[str, bytes]] = [("plain", s)]

    def hdr(label, fn):
        out.append((f"header {label}", edit_header(s, fn)))

    hdr("reserved flag bit", lambda b: b.__setitem__(1, b[1] | 0x04))
    hdr("two filters", lambda b: b.__setitem__(1, b[1] | 0x01))
    hdr("unknown filter", lambda b: b.__setitem__(2, 0x22))
    hdr("bcj before lzma2", lambda b: (b.__setitem__(1, b[1] | 0x01), b.__setitem__(slice(2, 2), bytes([0x04, 0x00]))))
    hdr("lzma2 props size 2", lambda b: b.__setitem__(3, 2))
    hdr("dictionary code 41", lambda b: b.__setitem__(4, 41))
    hdr("dictionary code 40", lambda b: b.__setitem__(4, 40))
    hdr("padding not zero", lambda b: b.__setitem__(len(b) - 1, 1))
    for label, c, u in (("sizes right", p.lzma2_len, len(plain)), ("csize +1", p.lzma2_len + 1, len(plain)), ("csize -1", p.lzma2_len - 1, len(plain)),
                        ("usize +1", p.lzma2_len, len(plain) + 1), ("usize -1", p.lzma2_len, len(plain) - 1), ("csize only", p.lzma2_len, None), ("usize only", None, len(plain)),
                        ("csize 0", 0, len(plain))):
        out.append((f"block sizes {label}", with_sizes(s, len(plain), c, u)))
    if p.pad:
        out.append(("block padding not zero", s[:p.data + p.lzma2_len] + b"\x01" + s[p.data + p.lzma2_len + 1:]))
    out.append(("block padding cut", s[:p.data + p.lzma2_len] + s[p.data + p.lzma2_len + p.pad:]))
    out += [("header csize overlong", s[:12] + block_header([(0x21, bytes([s[16]]))], p.lzma2_len, None, csize_raw=vli(p.lzma2_len)[:-1] + bytes([vli(p.lzma2_len)[-1] | 0x80, 0])) + s[12 + p.hs:])]
    for check in ("crc32", "crc64", "sha256"):
        c = xz(plain, check, preset=1)
        pc = Parsed(c)
        flipped = bytearray(c)
        flipped[pc.data + pc.lzma2_len + pc.pad] ^= 0x80
        out.append((f"check {check} field flipped", bytes(flipped)))
    out += mixed_chunks()
    out.append(("check type 2", retag(xz(plain, "crc32"), 2)))
    out.append(("check type 3", retag(xz(plain, "crc32"), 3)))
    out.append(("check type 6", retag(s, 6)))
    out.append(("check type 0 on a crc64 stream", retag(s, 0)))
    u0 = p.hs + p.lzma2_len + p.check
    for label, idx in (("right", make_index([(u0, len(plain))])), ("count +1", make_index([(u0, len(plain))], count=2)), ("count 0", make_index([], count=0)),
                       ("unpadded size +1", make_index([(u0 + 1, len(plain))])), ("usize +1", make_index([(u0, len(plain) + 1)])),
                       ("padding not zero", make_index([(u0, len(plain))], pad_byte=1)), ("two records", make_index([(u0, len(plain)), (u0, 1)]))):
        out.append((f"index {label}", with_index(s, idx)))
    out.append(("index crc wrong", s[:p.footer - 1] + bytes([s[p.footer - 1] ^ 1]) + s[p.footer:]))
    out.append(("footer backward size +1", edit_footer(s, lambda f: f.__setitem__(0, f[0] + 1))))
    out.append(("footer flags differ", edit_footer(s, lambda f: f.__setitem__(5, 1))))
    out.append(("footer crc wrong", s[:p.footer] + bytes([s[p.footer] ^ 1]) + s[p.footer + 1:]))
    out.append(("stream flags byte 0", s[:6] + b"\x01" + s[7:8] + crc(b"\x01" + s[7:8]) + s[12:]))
    out.append(("stream flags reserved", s[:6] + s[6:7] + bytes([s[7] | 0x10]) + crc(s[6:7] + bytes([s[7] | 0x10])) + s[12:]))
    out.append(("stream header crc", s[:8] + bytes([s[8] ^ 1]) + s[9:]))
    out.append(("stream header magic", b"\xfd7zXY\x00" + s[6:]))
    d = pn.data
    for label, off, val in (("first chunk without dict reset", 0, 0xC0), ("first chunk state reset only", 0, 0x80), ("control 3", 0, 3), ("control 0x7F", 0, 0x7F),
                            ("props lc+lp 5", 5, 13), ("props 225", 5, 225), ("range coder first byte", 6, 1), ("csize +1", 4, n[d + 4] + 1), ("csize -1", 4, n[d + 4] - 1),
                            ("usize +1", 2, n[d + 2] + 1), ("usize -1", 2, n[d + 2] - 1)):
        m = bytearray(n)
        m[d + off] = val & 0xFF
        out.append((f"lzma2 {label}", bytes(m)))
    m = bytearray(n)
    m[d + pn.lzma2_len - 1] = 0x02                                         # no end mark: a stored chunk header instead
    out.append(("lzma2 end mark replaced", bytes(m)))
    out.append(("lzma2 end mark missing", n[:d + pn.lzma2_len - 1] + bytes(8) + n[d + pn.lzma2_len:]))
    return out
