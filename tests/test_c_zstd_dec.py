#!/usr/bin/env python3
"""src/fs/zstd_dec.c, zstd_fse.c, zstd_huf.c, zstd_seq.c: the Zstandard decoder (RFC 8878) against the reference `zstd` command
(docs/ARCHIVE_ROADMAP_2026-11-24.md, phase 4).

ZS1 streams written by `zstd` (levels --fast=5 .. --ultra -22, with / without checksum and content size, tiny windows, --long=27, empty /
    raw / RLE / compressed blocks, text, random, zeros, binaries, block-size boundaries): exact bytes, CRC and consumed length
ZS2 several frames, skippable frames, an empty frame decode as one stream; bytes that start no frame are -EINVAL (after the frames before them)
ZS3 every proper prefix of small streams and sampled prefixes of large ones: -EINVAL, never success
ZS4 differential fuzz: flipped / rewritten / cut streams are accepted by `zstd -d` exactly when the lax build accepts them; the strict
    (product) build accepts a subset of those, with the reference's bytes - it also refuses reserved bits and Huffman streams with bits
    left over or missing, which libzstd's fast path lets through
ZS5 limits and errors: max_out is exact (-EFBIG), a sink error passes through, a dictionary and a window beyond 2^27 are -EOPNOTSUPP,
    a wrong content size or checksum is -EINVAL / -EBADMSG, a reserved bit / block type and a block beyond the window are -EINVAL
ZS6 scale: 40 MiB through a 1 KiB window (the history slides thousands of times), a match 20 MiB back (--long=27)
ZS7 aarch64 (qemu-user) gives the x86_64 results;  ZS8 mutation check
ZS9 hand-written blocks (tests/zstd_craft.py): every repeat-offset rule, overlapping matches, RLE literals, 2- and 3-byte sequence counts, invalid
    sequences; the reference decoder, an independent model and ours agree
"""

import random
import shutil
import struct
import sys
import zlib
from pathlib import Path

import c_harness as H
import zstd_craft as Z
from test_c_inflate_stream import EFBIG, EINVAL, ECANCELED, crc, record, run as run_records
from zstd_mut import test_mutants
from zstd_support import MAGIC, beyond_window, build, crafted_cases, frame_header, fuzz_cases, samples, text, zstd, zstd_d

EBADMSG, EOPNOTSUPP = -74, -95

def test_valid(exe: Path, smp: list[tuple[str, bytes, bytes]]) -> None:
    got = run_records(exe, [record(s) for _, _, s in smp])
    for (label, plain, s), r in zip(smp, got):
        assert r["rc"] == 0 and r["out"] == len(plain) and r["h"] == crc(plain) and r["in"] == len(s), (label, len(plain), len(s), r)
    print(f"[PASS] ZS1. {len(smp)} streams from zstd (all levels, windows, check / size flags, block kinds): exact bytes, CRC and length")


def skippable(payload: bytes, n: int = 0) -> bytes:
    return struct.pack("<II", 0x184D2A50 + n, len(payload)) + payload


def test_frames(exe: Path, smp: list[tuple[str, bytes, bytes]]) -> None:
    small = [(p, s) for _, p, s in smp if 0 < len(p) < 5000][:12]
    empty = next(s for _, p, s in smp if not p)
    recs, want = [], []
    for i in range(len(small) - 1):
        (p1, s1), (p2, s2) = small[i], small[i + 1]
        for st in (s1 + s2, s1 + skippable(b"meta" * i, i % 16) + s2, skippable(b"") + s1 + empty + s2 + skippable(b"x" * 100, 15)):
            recs.append(record(st))
            want.append((p1 + p2, len(st)))
    got = run_records(exe, recs)
    for (plain, used), r in zip(want, got):
        assert r["rc"] == 0 and r["out"] == len(plain) and r["h"] == crc(plain) and r["in"] == used, (len(plain), used, r)
    p1, s1 = small[0]
    bad = [s1 + b"junk", s1 + b"\0\0\0\0", s1 + MAGIC[:3], b"", b"\x28\xb5\x2f", s1[:4] + b"\xff" * 20, b"\x50\x2a\x4d\x18\x10\x00\x00\x00" + b"x" * 5]
    got = run_records(exe, [record(b) for b in bad])
    assert [r["rc"] for r in got] == [EINVAL] * len(bad), got
    assert got[0]["out"] == len(p1), "the frames before a bad byte are delivered"
    print(f"[PASS] ZS2. {len(recs)} multi-frame streams (skippable and empty frames inside) decode as one; {len(bad)} malformed tails are -EINVAL")


def test_truncation(exe: Path, smp: list[tuple[str, bytes, bytes]]) -> None:
    recs = []
    for _, plain, s in smp:
        if len(s) < 400:
            cuts = range(len(s))
        elif len(s) < 60000:
            cuts = range(0, len(s), max(1, len(s) // 150))
        else:
            cuts = range(0, len(s), max(1, len(s) // 12))
        recs += [record(s[:c]) for c in cuts]
    got = run_records(exe, recs)
    bad = [r for r in got if r["rc"] != EINVAL]
    assert not bad, f"{len(bad)} truncated streams were not -EINVAL: {bad[:3]}"
    print(f"[PASS] ZS3. {len(recs)} proper prefixes: all -EINVAL")


def test_fuzz(exe: Path, lax: Path, smp: list[tuple[str, bytes, bytes]]) -> None:
    rnd = random.Random(0xF22)
    cases = fuzz_cases(smp)
    recs = [record(c) for c in cases]
    strict, loose = run_records(exe, recs), run_records(lax, recs)
    accepted = stricter = 0
    for c, a, b in zip(cases, strict, loose):
        ref = zstd_d(c)
        if ref is None:
            assert b["rc"] != 0 and a["rc"] != 0, f"we accept what zstd rejects ({len(c)} bytes, hash {zlib.crc32(c):08x}): lax rc={b['rc']}"
            continue
        assert b["rc"] == 0 and b["out"] == len(ref), f"zstd accepts, the lax build does not: rc={b['rc']} out={b['out']} want={len(ref)}"
        if a["rc"] == 0:                                          # both accept: the bytes are the reference's
            assert a == b and a["h"] == crc(ref), (a, b)
            accepted += 1
        else:                                                     # only the strict build refuses: bits past the front of a stream, whose
            stricter += 1                                         # reading libzstd leaves undefined, so the bytes are not compared
    print(f"[PASS] ZS4. {len(cases)} mutated streams: the lax build equals `zstd -d` (verdict); the product build agrees on {accepted} and refuses {stricter} more")


def test_crafted(exe: Path, lax: Path) -> None:
    cases = crafted_cases()
    recs = [record(st) for _, st, _, _ in cases]
    got, got_lax = run_records(exe, recs), run_records(lax, recs)
    q = Z.sequences_section([(3, 3, 6)])
    reserved = Z.frame(Z.block(Z.literals_section("raw", b"abc") + q[:1] + bytes([q[1] | 1]) + q[2:]))
    assert zstd_d(reserved) == b"abcabc", "the reference must accept reserved bits it ignores"
    strict, loose = run_records(exe, [record(reserved)]), run_records(lax, [record(reserved)])
    assert strict[0]["rc"] == EINVAL and loose[0]["rc"] == 0 and loose[0]["h"] == crc(b"abcabc"), (strict, loose)
    bw = run_records(exe, [record(beyond_window())])[0], run_records(lax, [record(beyond_window())])[0]
    assert bw[0]["rc"] == EINVAL and bw[1]["rc"] == EINVAL, f"an offset beyond the window must be refused: {bw}"
    valid = stricter = 0
    for (label, st, want, want_lax), r, rl in zip(cases, got, got_lax):
        ref = zstd_d(st)
        assert ref == want_lax, f"{label}: the reference and the model disagree ({None if ref is None else len(ref)} vs {None if want_lax is None else len(want_lax)})"
        for res, w in ((r, want), (rl, want_lax)):
            if w is None:
                assert res["rc"] != 0, f"{label}: an invalid stream was accepted"
            else:
                assert res["rc"] == 0 and res["out"] == len(w) and res["h"] == crc(w), (label, res)
        valid += want is not None
        stricter += want is None and want_lax is not None
    print(f"[PASS] ZS9. {len(cases)} hand-written blocks ({valid} valid: repeat offsets, ll 0, overlaps, RLE literals, 32513 sequences, window mantissa; "
          f"{len(cases) - valid} refused, {stricter} of them only by the strict build): the reference, the model and the decoder agree; reserved mode bits are refused")


def test_limits(exe: Path, smp: list[tuple[str, bytes, bytes]]) -> None:
    label, plain, s = next((l, p, s) for l, p, s in smp if len(p) > 1000 and len(p) < 5000 and l.startswith("tiny") and l.endswith("-3"))
    n = len(plain)
    two = s + s
    r = run_records(exe, [record(two, max_out=2 * n), record(two, max_out=2 * n - 1), record(two, max_out=n + 5), record(s, max_out=0), record(s, fail_after=n // 2 + 1)])
    assert r[0]["rc"] == 0 and r[0]["out"] == 2 * n, r[0]
    assert r[1]["rc"] == EFBIG and r[1]["out"] == 2 * n - 1, r[1]
    assert r[2]["rc"] == EFBIG and r[2]["out"] == n + 5, r[2]
    assert r[3]["rc"] == EFBIG and r[3]["out"] == 0, r[3]
    assert r[4]["rc"] == ECANCELED, r[4]
    empty = bytes.fromhex("28b52ffd2000010000")
    dictid = frame_header(0x01, bytes([0x58, 0x07]) + bytes(20))              # Dictionary_ID_flag = 1: window byte, then ID 7
    wide = frame_header(0x00, bytes([18 << 3]) + bytes(20))                   # windowLog 10 + 18 = 28
    badfcs = bytearray(zstd(b"x" * 100, "--no-check", via_file=True))
    assert badfcs[4] & 0xC0 == 0 and badfcs[4] & 0x20, "single segment, FCS 1 byte"
    badfcs[5] = 101                                                             # claims 101 bytes, has 100
    badsum = bytearray(zstd(plain, "-3"))
    badsum[-1] ^= 1
    reserved = bytearray(s)
    reserved[4] |= 0x08                                                         # the reserved bit of the frame header descriptor
    btype3 = bytearray(s)
    btype3[6] |= 0x02                                                                           # the block type 2 (compressed) -> 3 (reserved)
    toobig = MAGIC + bytes([0x00, 0x00]) + struct.pack("<I", 1 | (2000 << 3))[:3] + bytes(2000)   # a 2000-byte block in a 1 KiB window
    for bad in (bytes(reserved), bytes(btype3), toobig):
        assert zstd_d(bad) is None, "the reference accepts a stream that is meant to be invalid"
    res = run_records(exe, [record(empty, max_out=0), record(dictid), record(wide), record(bytes(badfcs)), record(bytes(badsum)),
                            record(bytes(reserved)), record(bytes(btype3)), record(toobig)])
    assert res[0]["rc"] == 0 and res[0]["out"] == 0, res[0]
    assert res[1]["rc"] == EOPNOTSUPP and res[2]["rc"] == EOPNOTSUPP, res[1:3]
    assert res[3]["rc"] == EINVAL, res[3]
    assert res[4]["rc"] == EBADMSG, res[4]
    assert [r["rc"] for r in res[5:]] == [EINVAL] * 3, res[5:]
    print("[PASS] ZS5. max_out exact (-EFBIG), sink errors pass, dictionary / window beyond 2^27 are -EOPNOTSUPP, bad content size -EINVAL, bad checksum -EBADMSG")


def test_scale(exe: Path, work: Path) -> None:
    rnd = random.Random(99)
    big = text(rnd, 40 << 20)
    s1 = zstd(big, "-3", "--zstd=wlog=10", "--no-check")
    far = rnd.randbytes(20 << 20)
    plain2 = far + far[:1 << 20] + rnd.randbytes(1000)
    s2 = zstd(plain2, "-3", "--long=27", via_file=True)
    got = run_records(exe, [record(s1), record(s2)], timeout=600)
    assert got[0]["rc"] == 0 and got[0]["out"] == len(big) and got[0]["h"] == crc(big), got[0]
    assert got[1]["rc"] == 0 and got[1]["out"] == len(plain2) and got[1]["h"] == crc(plain2), got[1]
    print(f"[PASS] ZS6. 40 MiB through a 1 KiB window ({len(s1) >> 10} KiB packed) and a match 20 MiB back decode exactly")


def test_aarch64(x86: Path, work: Path, smp: list[tuple[str, bytes, bytes]]) -> None:
    if not H.available("aarch64"):
        print("[SKIP] ZS7. aarch64 toolchain / qemu-user missing")
        return
    pick = [s for _, p, s in smp if len(p) < 300000][::5]
    recs = [record(s) for s in pick] + [record(s[:len(s) // 2]) for s in pick] + [record(s + s) for s in pick[:10]]
    assert run_records(build(work / "a64", "aarch64"), recs, "aarch64", timeout=900) == run_records(x86, recs), "aarch64 and x86_64 disagree"
    print(f"[PASS] ZS7. {len(recs)} records give identical results under qemu-aarch64")



def main() -> None:
    if not shutil.which("zstd"):
        print("[SKIP] the zstd command is missing")
        return
    with H.workdir() as t:
        w = Path(t).resolve()
        exe = build(w / "x86")
        lax = build(w / "lax", lax=True)
        smp = samples()
        test_valid(exe, smp)
        test_frames(exe, smp)
        test_truncation(exe, smp)
        test_fuzz(exe, lax, smp)
        test_limits(exe, smp)
        test_scale(exe, w)
        test_crafted(exe, lax)
        test_aarch64(exe, w, smp)
        test_mutants(w, smp)
    print("ALL C ZSTD DECODER TESTS PASSED.")


if __name__ == "__main__":
    sys.exit(main())
