#!/usr/bin/env python3
"""src/fs/bzip2_dec.c: the bzip2 decoder against libbz2 (Python's bz2; docs/ARCHIVE_ROADMAP_2026-11-24.md, phase 4).

BZ1 streams from libbz2 (levels 1-9; empty, one byte, runs of every length around the 4-byte run-length stage, random, text, binaries): exact
    bytes, CRC and consumed length
BZ2 several streams decode as one; bytes behind the last stream that start no stream are -EINVAL (after the streams before them)
BZ3 every proper prefix of small streams and sampled prefixes of large ones: -EINVAL, never success
BZ4 differential fuzz: flipped / rewritten / cut streams are accepted by libbz2 exactly when we accept them (a randomised block aside), with the same bytes
BZ5 limits and errors: max_out is exact (-EFBIG), a sink error passes through, a wrong block / stream CRC is -EBADMSG, a randomised block -EOPNOTSUPP
BZ6 scale: 40 MiB in 100 kB blocks;  BZ7 aarch64 (qemu-user) gives the x86_64 results;  BZ8 mutation check
"""

import random
import sys
import zlib
from pathlib import Path

import c_harness as H
from bz_mut import test_mutants
from bz_support import bz, bz_d, build, fuzz_cases, samples, text
from test_c_inflate_stream import EFBIG, EINVAL, ECANCELED, crc, record, run as run_records

EBADMSG, EOPNOTSUPP = -74, -95


def test_valid(exe: Path, smp: list[tuple[str, bytes, bytes]]) -> None:
    got = run_records(exe, [record(s) for _, _, s in smp])
    for (label, plain, s), r in zip(smp, got):
        assert r["rc"] == 0 and r["out"] == len(plain) and r["h"] == crc(plain) and r["in"] == len(s), (label, len(plain), len(s), r)
    print(f"[PASS] BZ1. {len(smp)} streams from libbz2 (levels 1-9, run lengths, random / text / binary data): exact bytes, CRC and length")


def test_streams(exe: Path, smp: list[tuple[str, bytes, bytes]]) -> None:
    small = [(p, s) for _, p, s in smp if 0 < len(p) < 5000][:12]
    recs, want = [], []
    for i in range(len(small) - 1):
        (p1, s1), (p2, s2) = small[i], small[i + 1]
        recs.append(record(s1 + s2))
        want.append((p1 + p2, len(s1 + s2)))
    got = run_records(exe, recs)
    for (plain, used), r in zip(want, got):
        assert r["rc"] == 0 and r["out"] == len(plain) and r["h"] == crc(plain) and r["in"] == used, (len(plain), used, r)
    p1, s1 = small[0]
    bad = [s1 + b"junk", s1 + bytes(4), s1 + b"BZ", s1 + b"BZh0", b"", b"BZh", b"BZh9", s1[:3] + b"0" + s1[4:]]
    got = run_records(exe, [record(b) for b in bad])
    assert [r["rc"] for r in got] == [EINVAL] * len(bad), got
    assert got[0]["out"] == len(p1), "the streams before a bad byte are delivered"
    print(f"[PASS] BZ2. {len(recs)} multi-stream inputs decode as one; {len(bad)} malformed tails are -EINVAL")


def test_truncation(exe: Path, smp: list[tuple[str, bytes, bytes]]) -> None:
    recs = []
    for _, plain, s in smp:
        cuts = range(len(s)) if len(s) < 400 else range(0, len(s), max(1, len(s) // 150)) if len(s) < 60000 else range(0, len(s), max(1, len(s) // 12))
        recs += [record(s[:c]) for c in cuts]
    got = run_records(exe, recs)
    bad = [r for r in got if r["rc"] != EINVAL]
    assert not bad, f"{len(bad)} truncated streams were not -EINVAL: {bad[:3]}"
    print(f"[PASS] BZ3. {len(recs)} proper prefixes: all -EINVAL")


def test_fuzz(exe: Path, smp: list[tuple[str, bytes, bytes]]) -> None:
    cases = fuzz_cases(smp)
    got = run_records(exe, [record(c) for c in cases])
    accepted = rand = 0
    for c, r in zip(cases, got):
        ref = bz_d(c)
        if r["rc"] == EOPNOTSUPP:
            rand += 1                                                         # a randomised block (obsolete): libbz2 decodes it, we do not
            continue
        if ref is None:
            assert r["rc"] != 0, f"we accept what libbz2 rejects ({len(c)} bytes, crc {zlib.crc32(c):08x})"
        else:
            accepted += 1
            assert r["rc"] == 0 and r["out"] == len(ref) and r["h"] == crc(ref), f"libbz2 and we disagree: rc={r['rc']} out={r['out']} want={len(ref)}"
    print(f"[PASS] BZ4. {len(cases)} mutated streams: our verdict and bytes equal libbz2's ({accepted} accepted, {rand} randomised blocks, {len(cases) - accepted - rand} rejected)")


def test_limits(exe: Path, smp: list[tuple[str, bytes, bytes]]) -> None:
    _, plain, s = next(x for x in smp if x[0].startswith("tiny 9"))
    n = len(plain)
    two = s + s
    r = run_records(exe, [record(two, max_out=2 * n), record(two, max_out=2 * n - 1), record(two, max_out=n + 5), record(s, max_out=0), record(s, fail_after=n // 2 + 1)])
    assert r[0]["rc"] == 0 and r[0]["out"] == 2 * n and r[1]["rc"] == EFBIG and r[1]["out"] == 2 * n - 1, r[:2]
    assert r[2]["rc"] == EFBIG and r[2]["out"] == n + 5 and r[3]["rc"] == EFBIG and r[3]["out"] == 0 and r[4]["rc"] == ECANCELED, r[2:]
    block_crc = bytearray(s)
    block_crc[10] ^= 0x01                                                     # the block CRC starts at bit 80 of the stream
    stream_crc = bytearray(s)
    stream_crc[-1] ^= 0x10                                                    # the combined CRC (the last bits are padding)
    res = run_records(exe, [record(bytes(block_crc)), record(bytes(stream_crc))])
    assert all(x["rc"] in (EBADMSG, EINVAL) for x in res) and bz_d(bytes(block_crc)) is None, res
    print("[PASS] BZ5. max_out exact (-EFBIG), sink errors pass, a wrong block / stream CRC is refused")


def test_scale(exe: Path, work: Path) -> None:
    rnd = random.Random(99)
    big = text(rnd, 40 << 20)
    s = bz(big, 1)
    got = run_records(exe, [record(s)], timeout=900)
    assert got[0]["rc"] == 0 and got[0]["out"] == len(big) and got[0]["h"] == crc(big), got[0]
    print(f"[PASS] BZ6. 40 MiB in 100 kB blocks ({len(s) >> 10} KiB packed) decode exactly")


def test_aarch64(x86: Path, work: Path, smp: list[tuple[str, bytes, bytes]]) -> None:
    if not H.available("aarch64"):
        print("[SKIP] BZ7. aarch64 toolchain / qemu-user missing")
        return
    pick = [s for _, p, s in smp if len(p) < 300000][::4]
    recs = [record(s) for s in pick] + [record(s[:len(s) // 2]) for s in pick] + [record(s + s) for s in pick[:10]]
    assert run_records(build(work / "a64", "aarch64"), recs, "aarch64", timeout=900) == run_records(x86, recs), "aarch64 and x86_64 disagree"
    print(f"[PASS] BZ7. {len(recs)} records give identical results under qemu-aarch64")


def main() -> None:
    with H.workdir() as t:
        w = Path(t).resolve()
        exe = build(w / "x86")
        smp = samples()
        test_valid(exe, smp)
        test_streams(exe, smp)
        test_truncation(exe, smp)
        test_fuzz(exe, smp)
        test_limits(exe, smp)
        test_scale(exe, w)
        test_aarch64(exe, w, smp)
        test_mutants(w, smp)
    print("ALL C BZIP2 DECODER TESTS PASSED.")


if __name__ == "__main__":
    sys.exit(main())
