#!/usr/bin/env python3
"""src/fs/gzip_dec.c: RFC 1952 gzip on top of inflate_stream (docs/ARCHIVE_TOOLS_PLAN.md, phase 1).

GZ1 members from Python's gzip (levels 0-9, 0 B .. 3 MiB) and hand-made headers (FEXTRA, FNAME, FCOMMENT, FHCRC, every combination):
    bytes and CRC exact, `in` = the member length, 64 KiB chunks
GZ2 concatenated members decode as one stream (as gzip -d does); bytes behind the last member (zeros, text) are not consumed
GZ3 every proper prefix of members is rejected (-EINVAL), never accepted
GZ4 a wrong stored CRC32, a wrong ISIZE and a wrong header CRC are -EBADMSG; flipped data bytes are -EINVAL or -EBADMSG, never 0
GZ5 bad magic, method, reserved flag bits: -EINVAL; max_out holds across members (-EFBIG at exactly max_out); a sink error is passed on
GZ6 aarch64 (qemu-user) gives the x86_64 results;  GZ7 mutation check
"""

import gzip
import random
import struct
import subprocess
import sys
import zlib
from pathlib import Path

import c_harness as H
from test_c_inflate import raw_deflate, sample_inputs
from test_c_inflate_stream import record, run as run_records, crc, EINVAL, EFBIG, ECANCELED

ROOT = H.ROOT
SRC = "src/fs/gzip_dec.c"
SRCS = ["tests/harness/gzip_cli.c", SRC, "src/fs/inflate_stream.c", "src/fs/inflate.c", "src/core/hash.c", "src/core/string.c", "src/core/utf8.c",
        "src/core/memory.c", "src/core/syscall.c", "src/core/try_rt.c", "src/core/mem_prims.c"]
EBADMSG = -74
FHCRC, FEXTRA, FNAME, FCOMMENT = 2, 4, 8, 16


def build(work: Path, arch: str = "x86_64", srcs: list[str] | None = None) -> Path:
    work.mkdir(parents=True, exist_ok=True)
    return H.build(arch, work, srcs or SRCS, cflags=["-I", str(ROOT / "tests/harness")])


def member(plain: bytes, flags: int = 0, level: int = 6, extra: bytes = b"xy", name: bytes = b"file.txt", comment: bytes = b"a comment") -> bytes:
    """A gzip member written by hand so every header flag combination can be produced."""
    hdr = bytearray(struct.pack("<BBBBIBB", 0x1F, 0x8B, 8, flags, 0, 0, 3))
    if flags & FEXTRA:
        hdr += struct.pack("<H", len(extra)) + extra
    if flags & FNAME:
        hdr += name + b"\0"
    if flags & FCOMMENT:
        hdr += comment + b"\0"
    if flags & FHCRC:
        hdr += struct.pack("<H", zlib.crc32(bytes(hdr)) & 0xFFFF)
    return bytes(hdr) + raw_deflate(plain, level) + struct.pack("<II", crc(plain), len(plain) & 0xFFFFFFFF)


def samples() -> list[tuple[bytes, bytes]]:
    rnd = random.Random(0x6219)
    out = []
    for plain in sample_inputs(rnd) + [rnd.randbytes(200000) + b"tail" * 50000]:
        for level in (0, 1, 6, 9):
            out.append((plain, gzip.compress(plain, level, mtime=0)))
        for flags in range(32):
            if flags & 0xE0:
                continue
            if len(plain) < 5000 or flags in (31, 15):
                out.append((plain, member(plain, flags)))
    return out


def test_valid(exe: Path, smp: list[tuple[bytes, bytes]]) -> None:
    got = run_records(exe, [record(g) for _, g in smp])
    for (plain, g), r in zip(smp, got):
        assert r["rc"] == 0 and r["out"] == len(plain) and r["h"] == crc(plain) and r["in"] == len(g), (len(plain), r)
        assert r["short"] == 0, r
    print(f"[PASS] GZ1. {len(smp)} gzip members (all header flag combinations, levels 0-9): exact bytes, CRC and length")


def test_members(exe: Path, smp: list[tuple[bytes, bytes]]) -> None:
    small = [(p, g) for p, g in smp if 0 < len(p) < 5000][:20]
    recs, want = [], []
    for i in range(len(small) - 1):
        (p1, g1), (p2, g2) = small[i], small[i + 1]
        recs.append(record(g1 + g2))
        want.append((p1 + p2, len(g1) + len(g2)))
        recs.append(record(g1 + g2 + b"\0" * 37))
        want.append((p1 + p2, len(g1) + len(g2)))
        recs.append(record(g1 + b"trailing text"))
        want.append((p1, len(g1)))
    got = run_records(exe, recs)
    for (plain, used), r in zip(want, got):
        assert r["rc"] == 0 and r["out"] == len(plain) and r["h"] == crc(plain) and r["in"] == used, (len(plain), used, r)
    print(f"[PASS] GZ2. {len(recs)} multi-member / padded / trailing-text inputs: all members decoded, trailing bytes left alone")


def test_truncation(exe: Path, smp: list[tuple[bytes, bytes]]) -> None:
    recs = []
    for plain, g in smp:
        step = 1 if len(g) < 300 else max(1, len(g) // 40)
        recs += [record(g[:cut]) for cut in range(0, len(g), step)]
    got = run_records(exe, recs)
    bad = [r for r in got if r["rc"] != EINVAL]
    assert not bad, f"{len(bad)} truncated members were not rejected with -EINVAL: {bad[:3]}"
    print(f"[PASS] GZ3. {len(recs)} proper prefixes of members: all -EINVAL")


def test_corruption(exe: Path, smp: list[tuple[bytes, bytes]]) -> None:
    rnd = random.Random(0xC0)
    plain, g = next((p, g) for p, g in smp if 1000 < len(p) < 5000)
    crc_bad = bytearray(g)
    crc_bad[-8] ^= 1
    size_bad = bytearray(g)
    size_bad[-1] ^= 0x80
    hm = bytearray(member(plain, FHCRC | FNAME))
    hm[hm.index(b"\0") + 1] ^= 1                                     # the stored header CRC
    got = run_records(exe, [record(bytes(crc_bad)), record(bytes(size_bad)), record(bytes(hm))])
    assert [r["rc"] for r in got] == [EBADMSG] * 3, got
    flips = []
    for _ in range(600):
        b = bytearray(g)
        b[rnd.randrange(10, len(b) - 8)] ^= 1 << rnd.randrange(8)
        flips.append(record(bytes(b)))
    res = run_records(exe, flips)
    assert all(r["rc"] in (EINVAL, EBADMSG) for r in res), [r for r in res if r["rc"] not in (EINVAL, EBADMSG)][:3]
    print("[PASS] GZ4. wrong CRC32 / ISIZE / header CRC: -EBADMSG; 600 flipped data bytes: never accepted")


def test_misc(exe: Path, smp: list[tuple[bytes, bytes]]) -> None:
    plain, g = next((p, g) for p, g in smp if 1000 < len(p) < 5000)
    bad = [bytes([0x1F, 0x8C]) + g[2:], g[:2] + b"\x09" + g[3:], g[:3] + b"\x20" + g[4:], g[:3] + b"\x80" + g[4:], b"", b"\x1f\x8b"]
    got = run_records(exe, [record(b) for b in bad])
    assert all(r["rc"] == EINVAL for r in got), got
    two = g + g
    r = run_records(exe, [record(two, max_out=2 * len(plain)), record(two, max_out=2 * len(plain) - 1), record(two, max_out=len(plain) + 5),
                          record(two, fail_after=len(plain) + 1)])
    assert r[0]["rc"] == 0 and r[0]["out"] == 2 * len(plain), r[0]
    assert r[1]["rc"] == EFBIG and r[1]["out"] == 2 * len(plain) - 1, r[1]
    assert r[2]["rc"] == EFBIG and r[2]["out"] == len(plain) + 5, r[2]
    assert r[3]["rc"] == ECANCELED, r[3]
    print("[PASS] GZ5. bad magic / method / reserved flags: -EINVAL; max_out counts all members; sink errors pass through")


def test_aarch64(x86: Path, work: Path, smp: list[tuple[bytes, bytes]]) -> None:
    if not H.available("aarch64"):
        print("[SKIP] GZ6. aarch64 toolchain / qemu-user missing")
        return
    pick = [g for p, g in smp if len(p) < 100000][:40]
    recs = [record(g) for g in pick] + [record(g[:len(g) // 2]) for g in pick] + [record(g + g) for g in pick[:10]]
    assert run_records(build(work / "a64", "aarch64"), recs, "aarch64") == run_records(x86, recs), "aarch64 and x86_64 disagree"
    print(f"[PASS] GZ6. {len(recs)} records give identical results under qemu-aarch64")


MUTANTS = [
    ("crc check", "load_le32(p + tpos) != ~tee.crc", "false"),
    ("isize check", "load_le32(p + tpos + 4) != (uint32_t)tee.size", "false"),
    ("magic", "p[0] != 0x1F || p[1] != 0x8B || p[2] != 8", "p[0] != 0x1F || p[2] != 8"),
    ("reserved flags", "if (flg & 0xE0) return -EINVAL;", ""),
    ("fextra", "pos += xl;", "pos += xl + 1;"),
    ("fname loop", "for (uint32_t f = FNAME; f <= FCOMMENT; f <<= 1)", "for (uint32_t f = FNAME; f <= FNAME; f <<= 1)"),
    ("hcrc check", "if (want != got) return -EBADMSG;", ""),
    ("next member", "if (members && (src_len - pos < 2", "if (!members && (src_len - pos < 2"),
    ("max_out budget", "max_out - produced", "max_out"),
    ("consumed", "res->consumed = pos;", "res->consumed = tpos;"),
]


def test_mutants(work: Path, smp: list[tuple[bytes, bytes]]) -> None:
    text = (ROOT / SRC).read_text()
    for name, old, _ in MUTANTS:
        assert old in text, f"mutation anchor vanished: {name}"
    seen: set[bytes] = set()
    small = [(p, g) for p, g in smp if 0 < len(p) < 20000 and not (p in seen or seen.add(p))]       # one member per distinct content
    assert len(small) >= 6
    recs = [record(g) for _, g in small] + [record(small[i][1] + small[i + 1][1]) for i in range(len(small) - 1)]
    recs += [record(small[0][1] + b"zz"), record(small[1][1], max_out=3), record(small[2][1] + small[3][1], max_out=len(small[2][0]) + 1)]
    recs += [record(member(small[4][0], 31)), record(member(small[4][0], FHCRC)[:-9])]
    recs += [record(member(small[3][0], f)) for f in (FEXTRA, FEXTRA | FHCRC, FNAME, FCOMMENT | FHCRC)]       # a field right before the data
    g5 = small[5][1]
    for pos, bit in ((-8, 1), (-1, 0x80), (1, 1), (2, 1)):                                           # CRC, ISIZE, magic, method
        bad = bytearray(g5)
        bad[pos] ^= bit
        recs.append(record(bytes(bad)))
    bad2 = bytearray(member(small[1][0], FHCRC))
    bad2[10] ^= 1
    recs += [record(bytes(bad2)), record(g5[:3] + b"\xe0" + g5[4:]), record(g5[:3] + b"\x20" + g5[4:]), record(g5[:2] + b"\x08" + g5[3:])]
    base = run_records(build(work / "mbase"), recs)
    survivors: list[str] = []

    def job(i: int, name: str, old: str, new: str):
        def go() -> None:
            d = work / f"m{i}"
            d.mkdir()
            p = d / "gzip_dec.c"
            p.write_text(text.replace(old, new, 1))
            try:
                exe = H.build("x86_64", d, [s if s != SRC else str(p) for s in SRCS], cflags=["-I", str(ROOT / "tests/harness")])
                same = run_records(exe, recs, timeout=20) == base
            except (subprocess.CalledProcessError, subprocess.TimeoutExpired, AssertionError):
                same = False
            if same:
                survivors.append(name)
        return go

    H.parallel([job(i, *m) for i, m in enumerate(MUTANTS)])
    assert not survivors, f"mutants not noticed: {sorted(survivors)}"
    print(f"[PASS] GZ7. {len(MUTANTS)}/{len(MUTANTS)} mutants of gzip_dec.c are noticed")


def main() -> None:
    smp = samples()
    with H.workdir() as d:
        w = Path(d).resolve()
        exe = build(w / "x86")
        test_valid(exe, smp)
        test_members(exe, smp)
        test_truncation(exe, smp)
        test_corruption(exe, smp)
        test_misc(exe, smp)
        test_aarch64(exe, w, smp)
        test_mutants(w, smp)
    print("ALL C GZIP TESTS PASSED.")


if __name__ == "__main__":
    sys.exit(main())
