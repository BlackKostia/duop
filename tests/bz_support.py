"""Shared parts of the bzip2 decoder tests (tests/test_c_bzip2_dec.py): the harness build, the reference (Python's bz2 = libbz2), the sample corpus and
stream mutations."""

import bz2
import random
from pathlib import Path

import c_harness as H
from zstd_support import corpus as zstd_corpus, text

ROOT = H.ROOT
SRCS = ["tests/harness/bzip2_cli.c", "src/fs/bzip2_dec.c", "src/fs/inflate.c", "src/core/hash.c", "src/core/string.c", "src/core/utf8.c", "src/core/memory.c",
        "src/core/syscall.c", "src/core/try_rt.c", "src/core/mem_prims.c"]


def build(work: Path, arch: str = "x86_64", srcs: list[str] | None = None) -> Path:
    work.mkdir(parents=True, exist_ok=True)
    return H.build(arch, work, srcs or SRCS, cflags=["-I", str(ROOT / "tests/harness")])


def bz(data: bytes, level: int = 9) -> bytes:
    return bz2.compress(data, level)


def bz_d(stream: bytes) -> bytes | None:
    """What libbz2 makes of a stream (several streams included), None when it refuses it."""
    try:
        out = bytearray()
        while stream:
            d = bz2.BZ2Decompressor()
            out += d.decompress(stream)
            if not d.eof:
                return None
            stream = d.unused_data
        return bytes(out)
    except (OSError, ValueError, EOFError):
        return None


def corpus() -> dict[str, bytes]:
    rnd = random.Random(0xB22)
    c = {k: v for k, v in zstd_corpus().items() if k not in ("rawlit",)}
    runs = bytearray()
    for n in (1, 2, 3, 4, 5, 6, 255, 258, 259, 260, 263, 264, 300, 1000, 5000):               # the run-length stage of bzip2: 4 equal bytes + a count
        runs += bytes([rnd.randrange(256)]) * n + rnd.randbytes(3)
    c["runs"] = bytes(runs)
    c["runs4"] = b"aaaa" + b"b" * 4 + b"aaaa\x00a" + b"cccc\xffcccc\x04" * 10
    c["twobytes"] = bytes(rnd.choice((0, 1)) for _ in range(200000))
    return c


def samples() -> list[tuple[str, bytes, bytes]]:
    out = []
    for name, plain in corpus().items():
        levels = (1, 3, 5, 9) if len(plain) <= 300000 else (1, 9)
        for lv in levels:
            out.append((f"{name} {lv}", plain, bz(plain, lv)))
    return out


def mutations(rnd: random.Random, s: bytes, count: int) -> list[bytes]:
    out = []
    for _ in range(count):
        b = bytearray(s)
        kind = rnd.randrange(5)
        pos = rnd.randrange(len(b)) if rnd.random() < 0.5 else rnd.randrange(min(len(b), 120))
        if kind == 0:
            b[pos] ^= 1 << rnd.randrange(8)
        elif kind == 1:
            b[pos] = rnd.randrange(256)
        elif kind == 2 and len(b) > 8:
            del b[pos:pos + rnd.randrange(1, 6)]
        elif kind == 3:
            b[pos:pos] = rnd.randbytes(rnd.randrange(1, 4))
        else:
            for _ in range(3):
                b[rnd.randrange(len(b))] ^= 1 << rnd.randrange(8)
        out.append(bytes(b))
    return out


def fuzz_cases(smp: list[tuple[str, bytes, bytes]]) -> list[bytes]:
    rnd = random.Random(0xF22)
    pick = [s for label, p, s in smp if 20 < len(s) < 20000 and "random" not in label][::2][:40]
    return [m for s in pick for m in mutations(rnd, s, 60)]
