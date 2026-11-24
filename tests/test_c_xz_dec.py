#!/usr/bin/env python3
"""src/fs/xz_dec.c, lzma2_dec.c, lzma_dec.c: the .xz container with LZMA2 against liblzma (Python's lzma) and the `xz` command
(docs/ARCHIVE_ROADMAP_2026-11-24.md, phase 4).

XZ1 streams from liblzma (presets 0-9 and extreme, checks none / CRC-32 / CRC-64 / SHA-256, dictionaries from 4 KiB, every lc / lp / pb
    corner, stored chunks, empty / raw / zero / repeating data): exact bytes, CRC and consumed length
XZ2 several blocks (xz --block-size, threads), several streams, zero stream padding decode as one; bytes that start no stream are -EINVAL
XZ3 every proper prefix of small streams and sampled prefixes of large ones: -EINVAL, never success
XZ4 differential fuzz: flipped / rewritten / cut streams are accepted by liblzma exactly when we accept them, with the same bytes
XZ5 limits and errors: max_out is exact (-EFBIG), a sink error passes, a dictionary beyond XZ_DICT_MAX and the BCJ / Delta filters are -EOPNOTSUPP,
    wrong sizes / check / index / footer are -EINVAL / -EBADMSG
XZ6 scale: 40 MiB through a 4 KiB dictionary (the ring wraps thousands of times), a match 20 MiB back in a 32 MiB dictionary
XZ7 aarch64 (qemu-user) gives the x86_64 results;  XZ8 mutation check
XZ9 streams with one field edited and the CRCs fixed up (tests/xz_craft.py): header, sizes, padding, check types, index, footer, LZMA2 controls
"""

import lzma
import random
import shutil
import struct
import sys
import zlib
from pathlib import Path

import c_harness as H
from test_c_inflate_stream import EFBIG, EINVAL, ECANCELED, crc, record, run as run_records
from xz_craft import crafted
from xz_mut import test_mutants
from xz_support import MAGIC, block_header, build, fuzz_cases, samples, stream_header, text, xz, xz_cli, xz_d

EBADMSG, EOPNOTSUPP = -74, -95


def test_valid(exe: Path, smp: list[tuple[str, bytes, bytes]]) -> None:
    got = run_records(exe, [record(s) for _, _, s in smp])
    for (label, plain, s), r in zip(smp, got):
        assert r["rc"] == 0 and r["out"] == len(plain) and r["h"] == crc(plain) and r["in"] == len(s), (label, len(plain), len(s), r)
    print(f"[PASS] XZ1. {len(smp)} streams from liblzma (presets, checks, dictionaries, lc / lp / pb, block kinds): exact bytes, CRC and length")


def test_multi(exe: Path, smp: list[tuple[str, bytes, bytes]]) -> None:
    rnd = random.Random(11)
    big = text(rnd, 900000) + rnd.randbytes(200000) + text(rnd, 400000)
    cli = [xz_cli(big, "-1", "-T2", "--block-size=100KiB"), xz_cli(big, "-6", "--block-size=300KiB", "--check=sha256"), xz_cli(big, "-9", "-T4", "--block-size=1MiB")]
    small = [(p, s) for _, p, s in smp if 0 < len(p) < 5000][:10]
    recs, want = [record(c) for c in cli], [(big, len(c)) for c in cli]
    for i in range(len(small) - 1):
        (p1, s1), (p2, s2) = small[i], small[i + 1]
        for st in (s1 + s2, s1 + bytes(4 * (i + 1)) + s2, s1 + s2 + bytes(8)):
            recs.append(record(st))
            want.append((p1 + p2, len(st)))
    got = run_records(exe, recs)
    for (plain, used), r in zip(want, got):
        assert r["rc"] == 0 and r["out"] == len(plain) and r["h"] == crc(plain) and r["in"] == used, (len(plain), used, r)
    p1, s1 = small[0]
    bad = [s1 + b"junk", s1 + bytes(3), s1 + bytes(5), s1 + MAGIC[:4], b"", MAGIC, s1[:6] + bytes(6) + s1[12:]]
    got = run_records(exe, [record(b) for b in bad])
    assert [r["rc"] for r in got] == [EINVAL] * len(bad), got
    assert got[0]["out"] == len(p1), "the streams before a bad byte are delivered"
    print(f"[PASS] XZ2. {len(recs)} multi-block / multi-stream / padded inputs decode as one; {len(bad)} malformed tails are -EINVAL")


def test_truncation(exe: Path, smp: list[tuple[str, bytes, bytes]]) -> None:
    recs = []
    for _, plain, s in smp:
        cuts = range(len(s)) if len(s) < 400 else range(0, len(s), max(1, len(s) // 150)) if len(s) < 60000 else range(0, len(s), max(1, len(s) // 12))
        recs += [record(s[:c]) for c in cuts]
    got = run_records(exe, recs)
    bad = [r for r in got if r["rc"] != EINVAL]
    assert not bad, f"{len(bad)} truncated streams were not -EINVAL: {bad[:3]}"
    print(f"[PASS] XZ3. {len(recs)} proper prefixes: all -EINVAL")


def test_fuzz(exe: Path, smp: list[tuple[str, bytes, bytes]]) -> None:
    cases = fuzz_cases(smp)
    got = run_records(exe, [record(c) for c in cases])
    accepted = 0
    for c, r in zip(cases, got):
        ref = xz_d(c)
        if ref is None:
            assert r["rc"] != 0, f"we accept what liblzma rejects ({len(c)} bytes, crc {zlib.crc32(c):08x})"
        else:
            accepted += 1
            assert r["rc"] == 0 and r["out"] == len(ref) and r["h"] == crc(ref), f"liblzma and we disagree: rc={r['rc']} out={r['out']} want={len(ref)}"
    print(f"[PASS] XZ4. {len(cases)} mutated streams: our verdict and bytes equal liblzma's ({accepted} accepted, {len(cases) - accepted} rejected)")


def test_limits(exe: Path, smp: list[tuple[str, bytes, bytes]]) -> None:
    _, plain, s = next(x for x in smp if x[0].startswith("tiny crc64"))
    n = len(plain)
    two = s + s
    r = run_records(exe, [record(two, max_out=2 * n), record(two, max_out=2 * n - 1), record(two, max_out=n + 5), record(s, max_out=0), record(s, fail_after=n // 2 + 1)])
    assert r[0]["rc"] == 0 and r[0]["out"] == 2 * n and r[1]["rc"] == EFBIG and r[1]["out"] == 2 * n - 1, r[:2]
    assert r[2]["rc"] == EFBIG and r[2]["out"] == n + 5 and r[3]["rc"] == EFBIG and r[3]["out"] == 0 and r[4]["rc"] == ECANCELED, r[2:]
    bcj = lzma.compress(plain, format=lzma.FORMAT_XZ, filters=[{"id": lzma.FILTER_X86}, {"id": lzma.FILTER_LZMA2, "preset": 1}])
    delta = lzma.compress(plain, format=lzma.FORMAT_XZ, filters=[{"id": lzma.FILTER_DELTA, "dist": 2}, {"id": lzma.FILTER_LZMA2, "preset": 1}])
    huge = stream_header() + block_header([(0x21, bytes([40]))]) + bytes(20)                # dictionary code 40 = 4 GiB - 1
    unknown = stream_header() + block_header([(0x7E, b""), (0x21, bytes([20]))]) + bytes(20)
    assert xz_d(unknown) is None
    flipped = bytearray(s)
    flipped[-1] ^= 0x01                                                                      # the magic of the footer
    badidx = bytearray(s)
    badidx[len(s) - 12 - 6] ^= 0x01                                                          # a byte of the index
    res = run_records(exe, [record(bcj), record(delta), record(huge), record(unknown), record(bytes(flipped)), record(bytes(badidx))])
    assert res[0]["rc"] == EOPNOTSUPP and res[1]["rc"] == EOPNOTSUPP and res[2]["rc"] == EOPNOTSUPP, res[:3]
    assert res[3]["rc"] == EINVAL and res[4]["rc"] == EINVAL and res[5]["rc"] in (EINVAL, EBADMSG), res[3:]
    badsum = bytearray(xz(plain, "crc64"))
    pos = len(badsum) - 12 - 8 - 8 - 4                                                       # inside the CRC-64 of the block
    badsum[pos] ^= 0x01
    got = run_records(exe, [record(bytes(badsum))])[0]
    assert got["rc"] in (EBADMSG, EINVAL) and xz_d(bytes(badsum)) is None, got
    print("[PASS] XZ5. max_out exact (-EFBIG), sink errors pass, BCJ / Delta / huge dictionary are -EOPNOTSUPP, damaged footer / index / check are refused")


def crafted_cases() -> list[tuple[str, bytes]]:
    rnd = random.Random(5)
    out = []
    for name, plain in (("text", text(rnd, 3000)), ("random", rnd.randbytes(5000)), ("mixed", text(rnd, 4000) + rnd.randbytes(3000) + text(rnd, 2000))):
        out += [(f"{name}: {label}", st) for label, st in crafted(plain)]
    return out


def test_crafted(exe: Path) -> None:
    cases = crafted_cases()
    got = run_records(exe, [record(st) for _, st in cases])
    ok = limited = 0
    for (label, st), r in zip(cases, got):
        ref = xz_d(st)
        if ref is None:
            assert r["rc"] != 0, f"{label}: we accept what liblzma rejects"
        elif r["rc"] == EOPNOTSUPP:
            assert "dictionary code 40" in label, f"{label}: a feature or limit of ours (-EOPNOTSUPP) where liblzma decodes"
            limited += 1
        else:
            ok += 1
            assert r["rc"] == 0 and r["out"] == len(ref) and r["h"] == crc(ref), f"{label}: liblzma accepts, we answer {r}"
    print(f"[PASS] XZ9. {len(cases)} streams with one field edited (CRCs fixed up: header, index, footer, check types, sizes, LZMA2 controls): "
          f"our verdict equals liblzma's ({ok} accepted, {limited} over our dictionary limit)")


def test_scale(exe: Path, work: Path) -> None:
    rnd = random.Random(99)
    big = text(rnd, 40 << 20)
    s1 = xz(big, "crc32", dict_size=4096, preset=1)
    far = rnd.randbytes(20 << 20)
    plain2 = far + far[:1 << 20] + rnd.randbytes(1000)
    s2 = xz(plain2, "crc64", dict_size=32 << 20, preset=1)
    got = run_records(exe, [record(s1), record(s2)], timeout=900)
    assert got[0]["rc"] == 0 and got[0]["out"] == len(big) and got[0]["h"] == crc(big), got[0]
    assert got[1]["rc"] == 0 and got[1]["out"] == len(plain2) and got[1]["h"] == crc(plain2), got[1]
    print(f"[PASS] XZ6. 40 MiB through a 4 KiB dictionary ({len(s1) >> 10} KiB packed) and a match 20 MiB back decode exactly")


def test_aarch64(x86: Path, work: Path, smp: list[tuple[str, bytes, bytes]]) -> None:
    if not H.available("aarch64"):
        print("[SKIP] XZ7. aarch64 toolchain / qemu-user missing")
        return
    pick = [s for _, p, s in smp if len(p) < 300000][::5]
    recs = [record(s) for s in pick] + [record(s[:len(s) // 2]) for s in pick] + [record(s + s) for s in pick[:10]]
    assert run_records(build(work / "a64", "aarch64"), recs, "aarch64", timeout=900) == run_records(x86, recs), "aarch64 and x86_64 disagree"
    print(f"[PASS] XZ7. {len(recs)} records give identical results under qemu-aarch64")


def main() -> None:
    if not shutil.which("xz"):
        print("[SKIP] the xz command is missing")
        return
    with H.workdir() as t:
        w = Path(t).resolve()
        exe = build(w / "x86")
        smp = samples()
        test_valid(exe, smp)
        test_multi(exe, smp)
        test_truncation(exe, smp)
        test_fuzz(exe, smp)
        test_limits(exe, smp)
        test_crafted(exe)
        test_scale(exe, w)
        test_aarch64(exe, w, smp)
        test_mutants(w, smp)
    print("ALL C XZ DECODER TESTS PASSED.")


if __name__ == "__main__":
    sys.exit(main())
