"""A tiny Zstandard block writer for tests/test_c_zstd_dec.py: raw / RLE literals and sequences coded with the predefined FSE tables, so the
streams can hold exactly the sequences a test wants - repeat offsets, ll == 0, overlapping matches, 32512+ sequences, and invalid ones
(a literal run beyond the literals, an offset beyond the history). The states are found by inverting the decoding table, so the writer
shares no code with the decoder under test; `zstd -d` is the judge of what is valid."""

import struct

MAGIC = bytes.fromhex("28b52ffd")
LL_NORM = [4, 3, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 3, 2, 1, 1, 1, 1, 1, -1, -1, -1, -1]
ML_NORM = [1, 4, 3, 2, 2, 2, 2, 2, 2] + [1] * 37 + [-1] * 7
OF_NORM = [1, 1, 1, 1, 1, 1, 2, 2, 2] + [1] * 15 + [-1] * 5
LL_BITS = [0] * 16 + [1, 1, 1, 1, 2, 2, 3, 3, 4, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16]
LL_BASE = list(range(16)) + [16, 18, 20, 22, 24, 28, 32, 40, 48, 64, 128, 256, 512, 1024, 2048, 4096, 8192, 16384, 32768, 65536]
ML_BITS = [0] * 32 + [1, 1, 1, 1, 2, 2, 3, 3, 4, 4, 5, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16]
ML_BASE = list(range(3, 35)) + [35, 37, 39, 41, 43, 47, 51, 59, 67, 83, 99, 131, 259, 515, 1027, 2051, 4099, 8195, 16387, 32771, 65539]


def dtable(norm: list[int], log: int) -> list[list[int]]:
    """The decoding cells [symbol, nbits, base] (RFC 8878 4.1.1)."""
    size, mask = 1 << log, (1 << log) - 1
    cells: list[list[int]] = [[0, 0, 0] for _ in range(size)]
    high, nxt = size - 1, {}
    for s, c in enumerate(norm):
        if c == -1:
            cells[high][0] = s
            high -= 1
            nxt[s] = 1
        else:
            nxt[s] = c
    step, pos = (size >> 1) + (size >> 3) + 3, 0
    for s, c in enumerate(norm):
        for _ in range(max(c, 0)):
            cells[pos][0] = s
            pos = (pos + step) & mask
            while pos > high:
                pos = (pos + step) & mask
    for u in range(size):
        s = cells[u][0]
        ns = nxt[s]
        nxt[s] += 1
        nb = log - (ns.bit_length() - 1)
        cells[u][1], cells[u][2] = nb, (ns << nb) - size
    return cells


TABLES = {"ll": dtable(LL_NORM, 6), "of": dtable(OF_NORM, 5), "ml": dtable(ML_NORM, 6)}
LOGS = {"ll": 6, "of": 5, "ml": 6}


def states(cells: list[list[int]], syms: list[int]) -> tuple[list[int], list[int]]:
    """States x_i with symbol syms[i] and the bits that lead x_i to x_{i+1}: the table is a partition, so each predecessor is unique."""
    n = len(syms)
    x, bits = [0] * n, [0] * n
    x[n - 1] = next(u for u, c in enumerate(cells) if c[0] == syms[n - 1])
    for i in range(n - 2, -1, -1):
        for u, c in enumerate(cells):
            if c[0] == syms[i] and c[2] <= x[i + 1] < c[2] + (1 << c[1]):
                x[i], bits[i] = u, x[i + 1] - c[2]
                break
        else:
            raise ValueError("symbol has no probability")
    return x, bits


def code_of(base: list[int], value: int) -> int:
    return max(i for i, b in enumerate(base) if b <= value)


def sequences_section(seqs: list[tuple[int, int, int]], modes: int = 0, ll_zero: bool = False) -> bytes:
    """seqs = (literal length, match length, offset value) in the format's terms (offset value 1..3 = repeat codes, else offset + 3)."""
    n = len(seqs)
    head = bytes([n]) if n < 128 else bytes([128 + (n >> 8), n & 255]) if n < 0x7F00 else bytes([255]) + struct.pack("<H", n - 0x7F00)
    if not n:
        return b"\x00"
    tables, logs = (dict(TABLES, ll=[[0, 0, 0]]), dict(LOGS, ll=0)) if ll_zero else (TABLES, LOGS)     # ll_zero: the all-zero table (symbol 0, no bits)
    llc = [0 if ll_zero else code_of(LL_BASE, s[0]) for s in seqs]
    mlc = [code_of(ML_BASE, s[1]) for s in seqs]
    ofc = [s[2].bit_length() - 1 for s in seqs]
    xs = {k: states(tables[k], c) for k, c in (("ll", llc), ("of", ofc), ("ml", mlc))}
    fields: list[tuple[int, int]] = [(xs["ll"][0][0], logs["ll"]), (xs["of"][0][0], logs["of"]), (xs["ml"][0][0], logs["ml"])]
    for i, (ll, ml, ov) in enumerate(seqs):
        fields += [(ov - (1 << ofc[i]), ofc[i]), (ml - ML_BASE[mlc[i]], ML_BITS[mlc[i]]), (ll - LL_BASE[llc[i]], LL_BITS[llc[i]])]
        if i + 1 < n:
            for k in ("ll", "ml", "of"):
                x, bits = xs[k]
                fields.append((bits[i], tables[k][x[i]][1]))
    r = "".join(format(v, f"0{w}b") if w else "" for v, w in fields)
    stream = int("1" + r, 2).to_bytes((len(r) + 1 + 7) // 8, "little")
    return head + bytes([modes]) + stream


def literals_section(kind: str, data: bytes, size: int = 0) -> bytes:
    """kind 'raw': the bytes as they are; 'rle': one byte repeated `size` times."""
    n = len(data) if kind == "raw" else size
    t = 0 if kind == "raw" else 1
    if n < 32:
        h = bytes([t | n << 3])
    elif n < 4096:
        h = bytes([t | 1 << 2 | (n & 15) << 4, n >> 4])
    else:
        h = bytes([t | 3 << 2 | (n & 15) << 4, (n >> 4) & 255, n >> 12])
    return h + (data if kind == "raw" else data[:1])


def block(content: bytes, last: bool = True) -> bytes:
    return struct.pack("<I", int(last) | 2 << 1 | len(content) << 3)[:3] + content


def frame(blocks: bytes, window_desc: int = 0x58) -> bytes:
    return MAGIC + bytes([0x00, window_desc]) + blocks


def model(literals: bytes, seqs: list[tuple[int, int, int]], lax: bool = False) -> bytes:
    """What a conforming decoder produces; ValueError when a sequence is invalid (the decoder must refuse the stream). lax: a repeat offset
    that works out as zero becomes 1, as libzstd does."""
    out, lp, rep = bytearray(), 0, [1, 4, 8]
    for ll, ml, ov in seqs:
        if ll > len(literals) - lp:
            raise ValueError("literals")
        out += literals[lp:lp + ll]
        lp += ll
        if ov > 3:
            off = ov - 3
            rep = [off, rep[0], rep[1]]
        else:
            idx = ov - 1 + (ll == 0)
            off = rep[0] - 1 if idx == 3 else rep[idx]
            if off == 0:
                if not lax:
                    raise ValueError("zero offset")
                off = 1
            if idx:
                rep = [off, rep[0], rep[1]] if idx >= 2 else [off, rep[0], rep[2]]
        if off > len(out):
            raise ValueError("offset")
        for _ in range(ml):
            out.append(out[-off])
    return bytes(out + literals[lp:])
