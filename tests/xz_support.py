"""Shared parts of the xz / LZMA2 decoder tests (tests/test_c_xz_dec.py, tests/xz_mut.py): the harness build, the reference compressor (Python's
lzma = liblzma, and the `xz` command for several blocks), the sample corpus and stream mutations."""

import lzma
import random
import struct
import subprocess
import zlib
from pathlib import Path

import c_harness as H
from zstd_support import corpus as zstd_corpus, text

ROOT = H.ROOT
SRCS = ["tests/harness/xz_cli.c", "src/fs/xz_dec.c", "src/fs/lzma2_dec.c", "src/fs/lzma_dec.c", "src/fs/inflate.c", "src/core/hash.c", "src/core/hash_crc64.c",
        "src/core/string.c", "src/core/utf8.c", "src/core/memory.c", "src/core/syscall.c", "src/core/try_rt.c", "src/core/mem_prims.c"]
MAGIC = bytes.fromhex("fd377a585a00")
CHECKS = {"none": lzma.CHECK_NONE, "crc32": lzma.CHECK_CRC32, "crc64": lzma.CHECK_CRC64, "sha256": lzma.CHECK_SHA256}


def build(work: Path, arch: str = "x86_64", srcs: list[str] | None = None) -> Path:
    work.mkdir(parents=True, exist_ok=True)
    return H.build(arch, work, srcs or SRCS, cflags=["-I", str(ROOT / "tests/harness")])


def xz(data: bytes, check: str = "crc64", **lz) -> bytes:
    """One xz stream from liblzma: lz are the LZMA2 filter options (preset, dict_size, lc, lp, pb, mode, ...)."""
    return lzma.compress(data, format=lzma.FORMAT_XZ, check=CHECKS[check], filters=[{"id": lzma.FILTER_LZMA2, **({"preset": 6} | lz)}])


def xz_cli(data: bytes, *args: str) -> bytes:
    return subprocess.run(["xz", "-c", *args], input=data, capture_output=True, check=True).stdout


def xz_d(stream: bytes) -> bytes | None:
    """What liblzma makes of a stream, None when it refuses it (several streams and padding included)."""
    try:
        return lzma.decompress(stream, format=lzma.FORMAT_XZ)
    except lzma.LZMAError:
        return None


def corpus() -> dict[str, bytes]:
    rnd = random.Random(0x77A)
    c = {k: v for k, v in zstd_corpus().items() if k not in ("rawlit",)}
    c["lzmarep"] = (rnd.randbytes(3000) + b"".join(rnd.randbytes(12) + rnd.randbytes(400) * 3 for _ in range(30)))[:60000]          # matches, repeat matches
    c["bits"] = bytes(rnd.choice((0, 0, 0, 255, 1, 128)) for _ in range(150000))
    c["asm"] = b"".join(struct.pack("<II", 0xE8 | rnd.randrange(4), rnd.randrange(1 << 20)) for _ in range(20000))
    return c


def samples() -> list[tuple[str, bytes, bytes]]:
    out = []
    for name, plain in corpus().items():
        small = len(plain) <= 300000
        for check in ("crc64", "crc32", "none", "sha256") if small else ("crc64",):
            out.append((f"{name} {check}", plain, xz(plain, check)))
        opts = [dict(preset=0), dict(preset=1), dict(preset=3 | lzma.PRESET_EXTREME), dict(preset=9), dict(dict_size=4096), dict(dict_size=1 << 20, lc=0, lp=4, pb=0),
                dict(lc=4, lp=0, pb=4), dict(lc=1, lp=2, pb=1, mode=lzma.MODE_FAST), dict(lc=2, lp=1, pb=2, nice_len=273, depth=1000)]
        for o in opts if small else opts[:2] + opts[4:5]:
            out.append((f"{name} {sorted(o.items())}", plain, xz(plain, "crc64", **o)))
    return out


def mutations(rnd: random.Random, s: bytes, count: int) -> list[bytes]:
    out = []
    for _ in range(count):
        b = bytearray(s)
        kind = rnd.randrange(5)
        pos = rnd.randrange(len(b)) if rnd.random() < 0.5 else rnd.randrange(min(len(b), 80))
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
    pick = [s for label, p, s in smp if 100 < len(s) < 30000 and label.endswith(" none")][:30]            # no check: damaged data may still be accepted
    pick += [s for label, p, s in smp if 100 < len(s) < 30000 and not label.endswith(" none")][::6][:20]
    stored = [xz(rnd.randbytes(n), "none") for n in (50, 300, 3000, 20000)]                                 # stored chunks accept any byte
    return [m for s in pick + stored for m in mutations(rnd, s, 50)]


def vli(n: int) -> bytes:
    out = bytearray()
    while n >= 0x80:
        out.append(n & 0x7F | 0x80)
        n >>= 7
    return bytes(out + bytes([n]))


def stream_header(check: int = 1) -> bytes:
    flags = bytes([0, check])
    return MAGIC + flags + struct.pack("<I", zlib.crc32(flags))


def block_header(filters: list[tuple[int, bytes]], csize: int | None = None, usize: int | None = None, csize_raw: bytes | None = None) -> bytes:
    """A hand-made block header with the given filter chain [(id, props)]; csize_raw replaces the encoding of csize (to write an overlong one)."""
    flags = len(filters) - 1 | (0x40 if csize is not None else 0) | (0x80 if usize is not None else 0)
    body = bytes([flags]) + (csize_raw if csize_raw is not None else vli(csize) if csize is not None else b"") + (vli(usize) if usize is not None else b"")
    for fid, props in filters:
        body += vli(fid) + vli(len(props)) + props
    size = (len(body) + 1 + 4 + 3) // 4 * 4
    body = bytes([size // 4 - 1]) + body
    body += bytes(size - 4 - len(body))
    return body + struct.pack("<I", zlib.crc32(body))
