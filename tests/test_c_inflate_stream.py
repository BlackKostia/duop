#!/usr/bin/env python3
"""src/fs/inflate_stream.c: the streaming RFC 1951 decoder of the archive layer (docs/ARCHIVE_TOOLS_PLAN.md, phase 1).

IS1 valid streams (zlib levels 0/1/6/9, fixed, Huffman-only, RLE; sizes from 0 to 6 MiB): every byte arrives at the sink in 64 KiB
    chunks (only the last one may be shorter), the CRC equals the data, `in` is exactly the stream length
IS2 bytes behind the stream (a gzip trailer) are not consumed: `in` stops at the end of the final block
IS3 truncation: every proper prefix of small streams and sampled prefixes of large ones fail with -EINVAL, never succeed
IS4 max_out: a cap below the output stops with -EFBIG after exactly max_out bytes; the exact size passes; max_out 0 admits only ""
IS5 a sink error stops the decoder and is returned unchanged (-ECANCELED), with only the chunks delivered before it counted
IS6 malformed constructs: reserved block type, bad stored NLEN, over-subscribed code lengths, no end-of-block code, a distance before
    the start, distance codes 30 / 31 in plain DEFLATE, HLIT > 286, a code-length repeat with nothing to repeat
IS7 Deflate64 (7z): distances up to 64 KiB decode with INFLATE_DEFLATE64 and are rejected without it
IS8 differential against inflate_raw on 4000 mutated / random streams: whenever the streaming decoder succeeds, inflate_raw gives the
    same bytes; it must never succeed where the audited decoder fails
IS9 aarch64 (qemu-user): the same records give the same lines as x86_64
IS10 mutation check: each mutant of the decoder is noticed
"""

import random
import shutil
import struct
import subprocess
import sys
import tempfile
import zipfile
import zlib
from pathlib import Path

import c_harness as H
from test_c_inflate import raw_deflate, sample_inputs

ROOT = H.ROOT
SRC = "src/fs/inflate_stream.c"
SRCS = ["tests/harness/inflate_stream_cli.c", SRC, "src/fs/inflate.c", "src/core/hash.c", "src/core/string.c", "src/core/utf8.c",
        "src/core/memory.c", "src/core/syscall.c", "src/core/try_rt.c", "src/core/mem_prims.c"]
EINVAL, EFBIG, ECANCELED = -22, -27, -125
CHUNK = 65536


def record(stream: bytes, flags: int = 0, max_out: int = 2**64 - 1, fail_after: int = 0, mode: int = 0) -> bytes:
    return struct.pack("<IIQQI", mode, flags, max_out, fail_after, len(stream)) + stream


def run(exe: Path, recs: list[bytes], arch: str = "x86_64", timeout: int = 300) -> list[dict]:
    """One harness process per ~32 MiB of records (the harness buffers its whole stdin); results in record order."""
    out: list[dict] = []
    batch: list[bytes] = []
    size = 0
    for rec in recs + [None]:                                         # type: ignore[list-item]
        if rec is None or size + len(rec) > 32 * 1024 * 1024:
            if batch:
                res = H.run(arch, exe, [], b"".join(batch), timeout=timeout)
                assert res.returncode == 0, res.stderr[-300:]
                lines = res.stdout.decode().splitlines()
                assert len(lines) == len(batch), (len(lines), len(batch))
                out += [{k: int(v) for k, v in (kv.split("=") for kv in ln.split())} for ln in lines]
            batch, size = [], 0
        if rec is not None:
            batch.append(rec)
            size += len(rec)
    return out


def crc(data: bytes) -> int:
    return zlib.crc32(data) & 0xFFFFFFFF


def build(work: Path, arch: str = "x86_64", srcs: list[str] | None = None) -> Path:
    work.mkdir(parents=True, exist_ok=True)
    return H.build(arch, work, srcs or SRCS, cflags=["-I", str(ROOT / "tests/harness")])


class Bits:
    """LSB-first bit writer for hand-made DEFLATE streams."""

    def __init__(self) -> None:
        self.v = 0
        self.n = 0

    def put(self, value: int, nbits: int) -> "Bits":
        self.v |= value << self.n
        self.n += nbits
        return self

    def code(self, value: int, nbits: int) -> "Bits":          # Huffman codes go MSB first
        for i in range(nbits - 1, -1, -1):
            self.put((value >> i) & 1, 1)
        return self

    def bytes(self) -> bytes:
        return self.v.to_bytes((self.n + 7) // 8, "little")


def fixed_lit(b: Bits, sym: int) -> None:
    if sym < 144:
        b.code(0x30 + sym, 8)
    elif sym < 256:
        b.code(0x190 + sym - 144, 9)
    elif sym < 280:
        b.code(sym - 256, 7)
    else:
        b.code(0xC0 + sym - 280, 8)


def samples() -> list[tuple[bytes, bytes]]:
    rnd = random.Random(0x57EA)
    out = []
    for plain in sample_inputs(rnd):
        for level in (0, 1, 6, 9):
            out.append((plain, raw_deflate(plain, level)))
        out.append((plain, raw_deflate(plain, 9, zlib.Z_FIXED)))
        out.append((plain, raw_deflate(plain, 6, zlib.Z_HUFFMAN_ONLY)))
        out.append((plain, raw_deflate(plain, 6, zlib.Z_RLE)))
    big = b"".join(bytes([i % 251, (i * 7) % 253]) * (1 + i % 5) + rnd.randbytes(i % 9) for i in range(700000))
    assert len(big) >= 6 * 1024 * 1024, len(big)
    for level in (1, 6):
        out.append((big[:6 * 1024 * 1024], raw_deflate(big[:6 * 1024 * 1024], level)))
    out.append((b"\0" * 5000000, raw_deflate(b"\0" * 5000000, 9)))
    return out


def test_valid(exe: Path, smp: list[tuple[bytes, bytes]]) -> None:
    got = run(exe, [record(s) for _, s in smp])
    for (plain, stream), r in zip(smp, got):
        assert r["rc"] == 0 and r["out"] == len(plain) and r["h"] == crc(plain) and r["in"] == len(stream), (len(plain), r)
        assert r["short"] == 0 and r["chunks"] == -(-len(plain) // CHUNK), (len(plain), r)
    print(f"[PASS] IS1. {len(smp)} valid streams (0 B .. 6 MiB): bytes, CRC and consumed length exact; 64 KiB chunks, only the last shorter")


def test_trailer(exe: Path, smp: list[tuple[bytes, bytes]]) -> None:
    sub = [(p, s) for p, s in smp if len(p) < 100000]
    got = run(exe, [record(s + b"\x1f\x8b trailer 12345678") for _, s in sub])
    assert all(r["rc"] == 0 and r["in"] == len(s) for (_, s), r in zip(sub, got)), got[:3]
    print(f"[PASS] IS2. {len(sub)} streams followed by 20 foreign bytes: `in` ends exactly behind the final block")


def test_truncation(exe: Path, smp: list[tuple[bytes, bytes]]) -> None:
    recs, n = [], 0
    for plain, s in smp:
        step = 1 if len(s) < 400 else max(1, len(s) // (150 if len(s) < 100000 else 6))
        for cut in range(0, len(s), step):
            recs.append(record(s[:cut]))
    got = run(exe, recs)
    bad = [r for r in got if r["rc"] != EINVAL]
    assert not bad, f"{len(bad)} truncated streams were not rejected: {bad[:3]}"
    n = len(recs)
    print(f"[PASS] IS3. {n} proper prefixes of valid streams: all -EINVAL")


def test_limit_and_sink(exe: Path, smp: list[tuple[bytes, bytes]]) -> None:
    plain, s = next((p, s) for p, s in smp if len(p) == 6 * 1024 * 1024)
    cases = [(len(plain) - 1, 0), (CHUNK + 5, 0), (CHUNK, 0), (1, 0), (0, 0), (len(plain), 0), (2**64 - 1, 0)]
    got = run(exe, [record(s, max_out=m) for m, _ in cases])
    for (m, _), r in zip(cases, got):
        if m >= len(plain):
            assert r["rc"] == 0 and r["out"] == len(plain), (m, r)
        else:
            assert r["rc"] == EFBIG and r["out"] == m, (m, r)
    e = run(exe, [record(raw_deflate(b""), max_out=0), record(raw_deflate(b"x"), max_out=0)])
    assert e[0]["rc"] == 0 and e[1]["rc"] == EFBIG and e[1]["out"] == 0, e
    print("[PASS] IS4. max_out: -EFBIG after exactly max_out bytes, exact size passes, 0 admits only the empty output")
    got = run(exe, [record(s, fail_after=f) for f in (1, CHUNK, CHUNK + 1, 3 * CHUNK + 7)])
    for f, r in zip((1, CHUNK, CHUNK + 1, 3 * CHUNK + 7), got):
        assert r["rc"] == ECANCELED and r["out"] == (f // CHUNK) * CHUNK, (f, r)
    print("[PASS] IS5. a sink error stops the decoder and comes back unchanged; only delivered chunks are counted")


def malformed() -> list[tuple[str, bytes, int]]:
    out: list[tuple[str, bytes, int]] = []
    out.append(("block type 3", Bits().put(1, 1).put(3, 2).bytes(), 0))
    out.append(("stored NLEN", Bits().put(1, 1).put(0, 2).put(0, 5).put(4, 16).put(4, 16).bytes() + b"abcd", 0))
    b = Bits().put(1, 1).put(1, 2)                                  # fixed block, a match at distance 1 with nothing before it
    fixed_lit(b, 257)
    b.code(0, 5)
    fixed_lit(b, 256)
    out.append(("distance before start", b.bytes(), 0))
    b = Bits().put(1, 1).put(1, 2)
    fixed_lit(b, 65)
    fixed_lit(b, 257)
    b.code(30, 5).put(0, 13)                                        # distance code 30: only Deflate64 has it
    fixed_lit(b, 256)
    out.append(("distance code 30", b.bytes(), 0))
    b = Bits().put(1, 1).put(2, 2).put(30, 5).put(0, 5).put(0, 4)   # HLIT = 287
    out.append(("HLIT 287", b.put(0, 3 * 4).bytes(), 0))
    b = Bits().put(1, 1).put(2, 2).put(0, 5).put(0, 5).put(0, 4)    # 257 literals / 1 distance, code-length codes: three 1-bit codes
    for sym_len in (1, 1, 1, 0):                                    # order 16,17,18,0: 16, 17, 18 all length 1 -> over-subscribed
        b.put(sym_len, 3)
    out.append(("over-subscribed code-length code", b.bytes() + b"\0" * 8, 0))
    b = Bits().put(1, 1).put(2, 2).put(0, 5).put(0, 5).put(14, 4)   # 257 literal codes, 1 distance, 18 code-length-code lengths
    for i in range(18):
        b.put(1 if i in (3, 17) else 0, 3)                         # symbols 0 and 1 get a 1-bit code each
    for i in range(258):
        b.put(1 if i < 2 else 0, 1)                                # literals 0 and 1 have codes; 256 (end of block) does not
    out.append(("no end-of-block code", b.put(0, 1).bytes() + b"\0" * 8, 0))
    b = Bits().put(1, 1).put(2, 2).put(0, 5).put(0, 5).put(14, 4)  # literals 0, 1, 2 and 256 all get 1-bit codes: Kraft sum 2
    for i in range(18):
        b.put(1 if i in (3, 17) else 0, 3)
    for i in range(258):
        b.put(1 if i in (0, 1, 2, 256) else 0, 1)
    out.append(("over-subscribed literal code", b.put(0, 1).put(1, 1).bytes() + b"\0" * 8, 0))   # data: bit 0 = a literal, bit 1 = end of block
    b = Bits().put(1, 1).put(2, 2).put(0, 5).put(0, 5).put(0, 4)   # code-length alphabet {0, 16}
    for ln in (1, 0, 0, 1):
        b.put(ln, 3)
    out.append(("repeat with nothing to repeat", b.put(1, 1).put(0, 2).bytes() + b"\0" * 8, 0))
    return out


def test_malformed(exe: Path) -> None:
    cases = malformed()
    got = run(exe, [record(s) for _, s, _ in cases])
    for (name, _, _), r in zip(cases, got):
        assert r["rc"] == EINVAL, (name, r)
    print(f"[PASS] IS6. {len(cases)} malformed constructs rejected with -EINVAL")


def test_deflate64(exe: Path) -> None:
    if not shutil.which("7z"):
        print("[SKIP] IS7. 7z not installed")
        return
    rnd = random.Random(64)
    block = rnd.randbytes(50000)
    data = block + rnd.randbytes(3000) + block + bytes(range(256)) * 300      # a repeat at distance ~53 KB: needs the 64 KiB window
    with tempfile.TemporaryDirectory(prefix="d64_") as d:
        src = Path(d) / "payload.bin"
        src.write_bytes(data)
        z = Path(d) / "p.zip"
        subprocess.run(["7z", "a", "-tzip", "-mm=Deflate64", "-mx=9", str(z), str(src)], check=True, capture_output=True)
        raw = z.read_bytes()
        info = zipfile.ZipFile(z).infolist()[0]
        assert info.compress_type == 9, f"7z did not write Deflate64 (method {info.compress_type})"
        nlen, xlen = struct.unpack_from("<HH", raw, info.header_offset + 26)
        start = info.header_offset + 30 + nlen + xlen
        stream = raw[start:start + info.compress_size]
    on, off = run(exe, [record(stream, flags=1), record(stream, flags=0)])
    assert on["rc"] == 0 and on["out"] == len(data) and on["h"] == crc(data) and on["in"] == len(stream), on
    assert off["rc"] == EINVAL, f"without INFLATE_DEFLATE64 a distance beyond 32 KiB must fail: {off}"
    print(f"[PASS] IS7. Deflate64 stream of {len(stream)} B from 7z: exact with the flag, -EINVAL without it")


def test_differential(exe: Path, smp: list[tuple[bytes, bytes]]) -> None:
    rnd = random.Random(0xD1FF)
    small = [s for p, s in smp if 0 < len(s) < 6000]
    recs, srcs = [], []
    for s in small:
        for _ in range(100):
            b = bytearray(s)
            for _ in range(rnd.randrange(1, 4)):
                b[rnd.randrange(len(b))] ^= 1 << rnd.randrange(8)
            if rnd.random() < 0.2:
                del b[rnd.randrange(len(b)):]
            srcs.append(bytes(b))
    srcs += [rnd.randbytes(rnd.randrange(1, 300)) for _ in range(1200)]
    srcs = srcs[:4000]
    got = run(exe, [record(s, mode=1) for s in srcs])
    ok = 0
    for s, r in zip(srcs, got):
        if r["rc"] != 0:
            assert r["rc"] in (EINVAL, EFBIG), (s[:20], r)
            continue
        ok += 1
        assert r["raw"] == r["out"] and r["rh"] == r["h"], f"streaming succeeded where/with another result than inflate_raw: {r}"
    assert ok > 200, f"only {ok} of {len(srcs)} mutants decoded: the check would be vacuous"
    print(f"[PASS] IS8. {len(srcs)} mutated / random streams: {ok} decode (identical to inflate_raw), {len(srcs) - ok} rejected, never a disagreement")


def test_aarch64(x86: Path, work: Path, smp: list[tuple[bytes, bytes]]) -> None:
    if not H.available("aarch64"):
        print("[SKIP] IS9. aarch64 toolchain / qemu-user missing")
        return
    pick = [s for p, s in smp if len(p) < 300000][:30]
    recs = [record(s) for s in pick] + [record(s[:len(s) // 2]) for s in pick] + [record(s, max_out=100) for s in pick]
    a = run(build(work / "a64", "aarch64"), recs, "aarch64")
    b = run(x86, recs)
    assert a == b, "aarch64 and x86_64 disagree"
    print(f"[PASS] IS9. {len(recs)} records give identical results under qemu-aarch64")


# Equivalent, not listed: dropping "if (!all[256]) return -EINVAL;" - a block without an end-of-block code can only end by running out of
# input, which is -EINVAL as well; the check is kept for the early, zlib-compatible verdict.
MUTANTS = [
    ("distance vs start", "if (dist > st->flushed + st->wpos)", "if (dist > st->flushed + st->wpos + 1)"),
    ("truncation check", "st->bitcnt < st->virt * 8", "st->bitcnt < st->virt * 8 - 8"),
    ("limit clamp", "n = (uint32_t)(st->max_out - st->flushed);", "n = (uint32_t)(st->max_out - st->flushed + 1);"),
    ("flush at wrap", "if (st->wpos == WSIZE)", "if (st->wpos == WSIZE - 1)"),
    ("stored nlen", "(~nlen & 0xFFFFu) != len", "false"),
    ("oversubscribed", "if (left < 0) return -EINVAL;", ""),
    ("length extra", "len += take(st, len_ext[s]);", "len += 0;"),
    ("consumed", "(uint64_t)(st->src - st->base) - (uint64_t)real", "(uint64_t)(st->src - st->base)"),
]


def test_mutants(work: Path, smp: list[tuple[bytes, bytes]]) -> None:
    text = (ROOT / SRC).read_text()
    for name, old, _ in MUTANTS:
        assert old in text, f"mutation anchor vanished: {name}"
    rnd = random.Random(0xB1)
    rep = (b"abcdefgh" * 9 + rnd.randbytes(70)) * 2500                  # matches that run across the 64 KiB ring boundary
    pick = [s for p, s in smp if 0 < len(p) < 70000][:24] + [raw_deflate(rep, 6), raw_deflate(rep, 1), raw_deflate(rnd.randbytes(200000), 0)]
    recs = ([record(s, max_out=len(s) // 3) for s in pick] + [record(s) for s in pick] + [record(s[:-1]) for s in pick]
            + [record(s + b"zz") for s in pick] + [record(b, 0) for _, b, _ in malformed()] + [record(s, fail_after=5000) for s in pick])
    base = run(build(work / "mbase"), recs)
    survivors: list[str] = []

    def job(i: int, name: str, old: str, new: str):
        def go() -> None:
            d = work / f"m{i}"
            d.mkdir()
            p = d / "inflate_stream.c"
            p.write_text(text.replace(old, new, 1))
            try:
                exe = H.build("x86_64", d, [s if s != SRC else str(p) for s in SRCS], cflags=["-I", str(ROOT / "tests/harness")])
                same = run(exe, recs, timeout=20) == base                # a mutant that hangs is noticed too
            except (subprocess.CalledProcessError, subprocess.TimeoutExpired, AssertionError):
                same = False
            if same:
                survivors.append(name)
        return go

    H.parallel([job(i, *m) for i, m in enumerate(MUTANTS)])
    assert not survivors, f"mutants not noticed: {sorted(survivors)}"
    print(f"[PASS] IS10. {len(MUTANTS)}/{len(MUTANTS)} mutants of inflate_stream.c are noticed")


def main() -> None:
    smp = samples()
    with H.workdir() as d:
        w = Path(d).resolve()
        exe = build(w / "x86")
        test_valid(exe, smp)
        test_trailer(exe, smp)
        test_truncation(exe, smp)
        test_limit_and_sink(exe, smp)
        test_malformed(exe)
        test_deflate64(exe)
        test_differential(exe, smp)
        test_aarch64(exe, w, smp)
        test_mutants(w, smp)
    print("ALL C INFLATE STREAM TESTS PASSED.")


if __name__ == "__main__":
    sys.exit(main())
