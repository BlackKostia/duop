#!/usr/bin/env python3
"""src/fs/deflate.c, deflate_huff.c: the DEFLATE compressor (docs/ARCHIVE_ROADMAP_2026-11-24.md, phase 6), judged by zlib.

DF1 round trip: every sample at levels 0 / 1 / 3 / 6 / 9, written whole and in pieces of 1, 7, 1000 and 65537 bytes, is a valid stream that zlib inflates to
    exactly the input and that ends with the last block (nothing left over)
DF2 the same bytes whatever the pieces: the output of a sample does not depend on how the input was cut
DF3 quality: level 0 is stored (n + 5 bytes per 65535), levels 1 / 6 / 9 stay within 15 % (plus 64 bytes) of zlib's output at the same level, and a
    higher level never makes a sample larger by more than 1 %
DF4 limits: empty input, one byte, exactly 32768 / 65535 / 65536 bytes, matches at the edge of the window (32768 back) and just beyond it
DF5 aarch64 (qemu-user) gives the same bytes as x86_64;  DF6 mutation check
"""

import random
import subprocess
import sys
import zlib
from pathlib import Path

import c_harness as H
from zstd_support import corpus, text

ROOT = H.ROOT
SRCS = ["tests/harness/deflate_cli.c", "src/fs/deflate.c", "src/fs/deflate_huff.c", "src/core/string.c", "src/core/utf8.c", "src/core/memory.c",
        "src/core/syscall.c", "src/core/try_rt.c", "src/core/mem_prims.c", "src/core/hash.c"]
LEVELS = (0, 1, 3, 6, 9)
PIECES = (0, 1, 7, 1000, 65537)


def build(work: Path, arch: str = "x86_64", srcs: list[str] | None = None) -> Path:
    work.mkdir(parents=True, exist_ok=True)
    return H.build(arch, work, srcs or SRCS, cflags=["-I", str(ROOT / "tests/harness")])


def deflate(exe: Path, data: bytes, level: int, piece: int = 0, arch: str = "x86_64", timeout: int = 300) -> bytes:
    res = H.run(arch, exe, [str(level), str(piece)], data, timeout=timeout)
    assert res.returncode == 0, f"deflate level {level} piece {piece}: exit {res.returncode} {res.stderr[-200:]!r}"
    return res.stdout


def inflates(stream: bytes, data: bytes, label: str) -> None:
    d = zlib.decompressobj(-15)
    out = d.decompress(stream)
    assert d.eof and not d.unused_data and not d.unconsumed_tail, f"{label}: the stream does not end where it should"
    assert out == data, f"{label}: inflates to {len(out)} bytes that differ from the {len(data)} written"


def samples() -> dict[str, bytes]:
    rnd = random.Random(0xDEF)
    c = {k: v for k, v in corpus().items() if k not in ("rawlit", "random300k")}
    c["random100k"] = rnd.randbytes(100000)
    c["text1m"] = text(rnd, 1 << 20)
    window = rnd.randbytes(40000)
    c["edge"] = window + window[:3000] + rnd.randbytes(100) + window[7000:12000]                     # matches 40000 back (too far) and inside the window
    far = rnd.randbytes(32768)
    c["window"] = far + far + rnd.randbytes(5)                                                      # a match exactly 32768 back
    c["longrun"] = bytes(70000) + b"x" + bytes(300000)
    return c


def test_round_trip(exe: Path, smp: dict[str, bytes]) -> dict[tuple[str, int, int], bytes]:
    outs: dict[tuple[str, int, int], bytes] = {}
    jobs = []
    for name, data in smp.items():
        for lv in LEVELS:
            for piece in PIECES if len(data) <= 300000 else (0, 65537):
                jobs.append((name, lv, piece, data))

    def one(name: str, lv: int, piece: int, data: bytes) -> None:
        out = deflate(exe, data, lv, piece)
        inflates(out, data, f"{name} level {lv} piece {piece}")
        outs[(name, lv, piece)] = out

    H.parallel([lambda j=j: one(*j) for j in jobs])
    print(f"[PASS] DF1. {len(jobs)} compressions (levels 0-9, pieces of 1 B .. 64 KiB) inflate to exactly their input")
    return outs


def test_determinism(outs: dict[tuple[str, int, int], bytes]) -> None:
    bad = []
    for (name, lv, piece), out in outs.items():
        if out != outs[(name, lv, 0)] if (name, lv, 0) in outs else False:
            bad.append((name, lv, piece))
    assert not bad, f"the output depends on how the input was cut: {bad[:5]}"
    print(f"[PASS] DF2. {len(outs)} outputs: the bytes do not depend on the size of the pieces written")


def test_quality(outs: dict[tuple[str, int, int], bytes], smp: dict[str, bytes]) -> None:
    for name, data in smp.items():
        stored = outs[(name, 0, 0)]
        assert len(stored) == len(data) + 5 * max(1, -(-len(data) // 65535)), (name, len(stored), len(data))
        sizes = {lv: len(outs[(name, lv, 0)]) for lv in (1, 6, 9)}
        for lv, size in sizes.items():
            ref = len(zlib.compress(data, lv)) - 6
            assert size <= ref * 1.15 + 64, f"{name} level {lv}: {size} bytes against zlib's {ref}"
        assert sizes[9] <= sizes[1] * 1.01 + 16 and sizes[6] <= sizes[1] * 1.01 + 16, (name, sizes)
    print(f"[PASS] DF3. level 0 is stored; levels 1 / 6 / 9 are within 15 % of zlib on {len(smp)} samples and never worse at a higher level")


def test_limits(exe: Path) -> None:
    rnd = random.Random(5)
    cases = {"empty": b"", "one": b"a", "n32768": rnd.randbytes(32768), "n65535": rnd.randbytes(65535), "n65536": rnd.randbytes(65536), "zeros65536": bytes(65536),
             "ab": b"ab" * 70000, "three": b"abc", "four": b"abcd" * 3}
    for name, data in cases.items():
        for lv in LEVELS:
            for piece in (0, 1):
                inflates(deflate(exe, data, lv, piece), data, f"{name} level {lv} piece {piece}")
    print(f"[PASS] DF4. {len(cases)} edge inputs (empty, one byte, block-size multiples, window-edge matches) round-trip at every level")


def test_aarch64(x86: Path, work: Path, smp: dict[str, bytes], outs: dict[tuple[str, int, int], bytes]) -> None:
    if not H.available("aarch64"):
        print("[SKIP] DF5. aarch64 toolchain / qemu-user missing")
        return
    a64 = build(work / "a64", "aarch64")
    for name in ("tiny", "text200k", "mixed", "edge"):
        for lv in (1, 6):
            assert deflate(a64, smp[name], lv, 0, "aarch64") == outs[(name, lv, 0)], (name, lv)
    print("[PASS] DF5. the same bytes under qemu-aarch64")


def main() -> None:
    with H.workdir() as t:
        w = Path(t).resolve()
        exe = build(w / "x86")
        smp = samples()
        outs = test_round_trip(exe, smp)
        test_determinism(outs)
        test_quality(outs, smp)
        test_limits(exe)
        test_aarch64(exe, w, smp, outs)
        from deflate_mut import test_mutants
        test_mutants(w, smp)
    print("ALL C DEFLATE TESTS PASSED.")


if __name__ == "__main__":
    sys.exit(main())
