"""Shared parts of the Zstandard decoder tests (tests/test_c_zstd_dec.py, tests/zstd_mut.py): building the harness, the reference compressor /
decompressor (the `zstd` command), the sample corpus, stream mutations and the hand-written blocks of tests/zstd_craft.py."""

import random
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

import c_harness as H
import zstd_craft as Z

ROOT = H.ROOT
SRCS = ["tests/harness/zstd_cli.c", "src/fs/zstd_dec.c", "src/fs/zstd_fse.c", "src/fs/zstd_huf.c", "src/fs/zstd_seq.c", "src/fs/inflate.c", "src/core/hash.c",
        "src/core/hash_xxh.c", "src/core/string.c", "src/core/utf8.c", "src/core/memory.c", "src/core/syscall.c", "src/core/try_rt.c", "src/core/mem_prims.c"]
EBADMSG, EOPNOTSUPP = -74, -95
MAGIC = bytes.fromhex("28b52ffd")
WORDS = [b"archive", b"stream", b"window", b"huffman", b"literal", b"sequence", b"offset", b"duop", b"panel", b"file", b"zstd", b"frame", b"block"]


def build(work: Path, arch: str = "x86_64", srcs: list[str] | None = None, lax: bool = False) -> Path:
    """lax: ZSTD_LAX, the build that tolerates exactly what libzstd tolerates (see duop_zstd_int.h) - for the bit-exact differential fuzz."""
    work.mkdir(parents=True, exist_ok=True)
    return H.build(arch, work, srcs or SRCS, cflags=["-I", str(ROOT / "tests/harness"), *(["-DZSTD_LAX=1"] if lax else [])])


def zstd(data: bytes, *args: str, via_file: bool = False) -> bytes:
    """`zstd -c` of data; via_file records the content size and may use a single-segment frame."""
    if via_file:
        with tempfile.NamedTemporaryFile() as f:
            f.write(data)
            f.flush()
            return subprocess.run(["zstd", "-q", "-c", *args, f.name], capture_output=True, check=True).stdout
    return subprocess.run(["zstd", "-q", "-c", *args], input=data, capture_output=True, check=True).stdout


def zstd_d(stream: bytes) -> bytes | None:
    r = subprocess.run(["zstd", "-q", "-d", "-c"], input=stream, capture_output=True)
    return r.stdout if r.returncode == 0 else None


def text(rnd: random.Random, n: int) -> bytes:
    out = bytearray()
    while len(out) < n:
        out += rnd.choice(WORDS) + (b"\n" if rnd.random() < 0.1 else b" ") + b"%d" % rnd.randrange(1000) + b" "
    return bytes(out[:n])


def binary(n: int) -> bytes:
    for p in ("/usr/bin/xz", "/bin/ls", "/usr/bin/zstd", sys.executable):
        if Path(p).is_file():
            return Path(p).read_bytes()[:n]
    return bytes(range(256)) * (n // 256)


def corpus() -> dict[str, bytes]:
    rnd = random.Random(0x25D)
    mixed = text(rnd, 70000) + rnd.randbytes(50000) + bytes(40000) + text(rnd, 30000) + binary(60000)
    alpha = bytes(rnd.choice(range(16)) for _ in range(120000))                     # 16 equally likely symbols: Huffman weights written as they are
    rawlit = rnd.randbytes(5000) + b"".join(rnd.randbytes(6) + s for s in (b"ab" * 40, b"xyz" * 33)) * 40
    return {"alpha16": alpha, "rawlit": rawlit, "per4": rnd.randbytes(60) + b"abcd" * 30000, "per8": rnd.randbytes(60) + b"abcdefgh" * 15000,
            "empty": b"", "one": b"a", "short": b"hello zstd\n" * 3, "text200k": text(rnd, 200000), "random300k": rnd.randbytes(300000), "zeros1m": bytes(1 << 20),
            "pattern": bytes(range(256)) * 2000, "elf": binary(900000), "b127": text(rnd, 127), "b128k": text(rnd, 131072), "b128k1": text(rnd, 131073),
            "b256k": text(rnd, 262144), "mixed": mixed, "rep7": (b"abcdefg" * 200000)[:1000000], "tiny": text(rnd, 2000)}


def samples() -> list[tuple[str, bytes, bytes]]:
    """(label, plain, stream) from the reference compressor."""
    out = []
    data = corpus()
    quick = [[], ["-1"], ["--fast=5"], ["-3"], ["--no-check"], ["--zstd=wlog=10"], ["-9"], ["-T2", "-5"], ["--zstd=wlog=11,strat=1"]]
    slow = [["-19"], ["--ultra", "-22"], ["--long=27", "-12"], ["--zstd=strat=9,wlog=18"]]
    for name, plain in data.items():
        for args in quick + (slow if len(plain) <= 300000 else [["-15"]]):
            for via in (False, True):
                if via and args != [] and args != ["--no-check"] and args != ["-19"]:
                    continue
                out.append((f"{name} {' '.join(args)}{' file' if via else ''}", plain, zstd(plain, *args, via_file=via)))
    return out


def mutations(rnd: random.Random, s: bytes, count: int) -> list[bytes]:
    out = []
    for _ in range(count):
        b = bytearray(s)
        kind = rnd.randrange(5)
        pos = rnd.randrange(len(b)) if rnd.random() < 0.5 else rnd.randrange(min(len(b), 90))      # half of them in the headers and tables
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
    pick = [s for label, p, s in smp if 200 < len(s) < 30000 and "random" not in label][::2][:40]
    return [m for s in pick for m in mutations(rnd, s, 60)]


def raw_block(payload: bytes, last: bool = True) -> bytes:
    return struct.pack("<I", int(last) | len(payload) << 3)[:3] + payload


def models(data: bytes, seqs: list[tuple[int, int, int]], lax: bool) -> bytes | None:
    try:
        return Z.model(data, seqs, lax)
    except ValueError:
        return None


def crafted_cases() -> list[tuple[str, bytes, bytes | None, bytes | None]]:
    """(label, stream, bytes a strict decoder produces or None, bytes libzstd produces or None), from tests/zstd_craft.py's block writer."""
    rnd = random.Random(0xC4AF7)
    out: list[tuple[str, bytes, bytes | None, bytes | None]] = []
    for t in range(500):
        kind = "rle" if t % 7 == 3 else "raw"
        lits = bytes([rnd.randrange(256)]) if kind == "rle" else rnd.randbytes(rnd.randrange(8, 90))
        size = rnd.randrange(8, 120) if kind == "rle" else len(lits)
        data = lits * size if kind == "rle" else lits
        seqs, cur = [], 0
        for _ in range(rnd.randrange(1, 9)):
            ll = rnd.choice([0, 0, 1, 2, rnd.randrange(0, 12)])
            ov = rnd.choice([1, 2, 3, 3, rnd.randrange(4, 3 + max(2, cur + ll))])
            ml = rnd.choice([3, 4, 5, rnd.randrange(3, 40), rnd.randrange(3, 300)])
            seqs.append((ll, ml, ov))
            cur += ll + ml
        body = Z.literals_section(kind, lits, size) + Z.sequences_section(seqs)
        out.append((f"random {t} {kind}", Z.frame(Z.block(body)), models(data, seqs, False), models(data, seqs, True)))
    base3 = [(3, 3, 6)] + [(0, 3, 6)] * 40000
    for n in (1, 127, 128, 255, 256, 32511, 32512, 32513, 40000):
        seqs = base3[:n]
        w = Z.model(b"abc", seqs)
        out.append((f"{n} sequences", Z.frame(Z.block(Z.literals_section("raw", b"abc") + Z.sequences_section(seqs))), w, w))
    ok = Z.literals_section("raw", b"abcdefgh") + Z.sequences_section([(8, 8, 11), (0, 4, 1)])
    w = Z.model(b"abcdefgh", [(8, 8, 11), (0, 4, 1)])
    out.append(("repeat after ll 0", Z.frame(Z.block(ok)), w, w))
    out.append(("mantissa 7", MAGIC + bytes([0x00, 0x07]) + raw_block(bytes(1500)), bytes(1500), bytes(1500)))
    out.append(("no sequences, a stray byte", Z.frame(Z.block(Z.literals_section("raw", b"abc") + b"\x00\x00")), None, None))
    seq1 = Z.sequences_section([(3, 3, 6)])
    out.append(("LL repeat before any table", Z.frame(Z.block(Z.literals_section("raw", b"abc") + seq1[:1] + bytes([0xC0]) + seq1[2:])), None, None))
    out.append(("LL symbol out of range", Z.frame(Z.block(Z.literals_section("raw", b"abc") + seq1[:1] + bytes([0x40, 40]) + seq1[2:])), None, None))
    two = raw_block(b"abcdefgh", last=False)
    zero = Z.sequences_section([(0, 4, 6)], modes=0xC0, ll_zero=True)                  # valid for a zero table: Repeat_Mode with none set yet
    out.append(("repeat mode in the first block", Z.frame(two + Z.block(Z.literals_section("raw", b"xy") + zero)), None, None))
    return out


def beyond_window() -> bytes:
    """An offset past the window but inside the data held: libzstd's answer depends on its buffer, ours is a fixed 'corrupt' (the history
    that survives a slide is exactly one window, so accepting it would depend on when the buffer moved)."""
    far = Z.literals_section("raw", b"wxyz") + Z.sequences_section([(4, 8, 1500 + 3)])
    return MAGIC + bytes([0x00, 0x00]) + raw_block(bytes(range(256)) * 4, last=False) + raw_block(bytes(range(256)) * 4, last=False) + Z.block(far)


def frame_header(desc: int, rest: bytes) -> bytes:
    return MAGIC + bytes([desc]) + rest
