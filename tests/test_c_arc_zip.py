#!/usr/bin/env python3
"""src/fs/arc_zip.c: the zip parser of the archive layer (docs/ARCHIVE_TOOLS_PLAN.md, phase 1) - end record from the back, ZIP64, prefixes
(self-extractors), UTF-8 / CP437 names, Info-ZIP unicode paths, extended timestamps, Unix modes and symlinks, encryption flags,
local-header validation and overlapping members.

AZ1 the fixture corpus (deflate / stored / ZIP64 / bzip2 / lzma, links, comment, CP437, SFX, encrypted, overlap, empty): every member's
    name, kind, sizes, method, CRC, local-header offset, mtime and mode equal what Python's zipfile reads
AZ2 hand-made archives: ZIP64 central records and end record, a fake end record inside the comment, 0x5455 / 0x7075 (right and wrong
    CRC) / 0x9901 extras, DOS and Unix attributes, '..' and absolute names, CP437 without the flag, UTF-8 without the flag
AZ3 damage: a missing local header drops the member (damaged), data beyond the file is clamped, a short central directory is damaged,
    multi-disk is -EOPNOTSUPP, non-zips are -EINVAL, overlapping members are flagged unsafe
AZ4 1500 mutants and forged size / offset / count fields read at the edge of a guard page: never a fault, never data outside the file
AZ5 aarch64 (qemu-user) equals x86_64;  AZ6 mutation check
"""

import calendar
import random
import struct
import subprocess
import sys
import zipfile
import zlib
from pathlib import Path

import c_harness as H
from archive_fixtures import build_corpus
from test_c_arc_index import DIR, normalize
from test_c_arc_tar import SYM, UNSAFE, SPECIAL, parse

ROOT = H.ROOT
SRC = "src/fs/arc_zip.c"
SRCS = ["tests/harness/arcparse_cli.c", SRC, "src/fs/arc_index.c", "src/core/hash.c", "src/core/string.c", "src/core/utf8.c",
        "src/core/memory.c", "src/core/syscall.c", "src/core/try_rt.c", "src/core/mem_prims.c"]
ENC = 8
CFLAGS = ["-I", str(ROOT / "tests/harness"), "-DWITH_ZIP"]


def build(work: Path, arch: str = "x86_64", srcs: list[str] | None = None) -> Path:
    work.mkdir(parents=True, exist_ok=True)
    return H.build(arch, work, srcs or SRCS, cflags=CFLAGS)


def zparse(exe: Path, paths: list[Path], arch: str = "x86_64", timeout: int = 120) -> list[list[str]]:
    return parse(exe, paths, arch, "Z", timeout)


def dos(dt: tuple) -> tuple[int, int]:
    return (dt[3] << 11 | dt[4] << 5 | dt[5] // 2), ((dt[0] - 1980) << 9 | dt[1] << 5 | dt[2])


def want(name: bytes, flags: int = 0, size: int = 0, comp: int = 0, off: int = 0, mtime: int = 0, mode: int = 0o644, method: int = 0, crc: int = 0) -> tuple:
    n, unsafe, _ = normalize(name, flags)
    fl = flags | (DIR if n.endswith(b"/") else 0) | (UNSAFE if unsafe else 0)
    return n, f"{n.hex()} {fl} {size} {comp} {off} {mtime} {mode} {method} {crc} -"


def lines(entries: list[tuple]) -> list[str]:
    last = {e[0]: e[1] for e in entries}
    return [last[k] for k in sorted(last)]


def from_zipfile(path: Path) -> list[str]:
    ents = []
    with zipfile.ZipFile(path) as zf:
        for i in zf.infolist():
            raw = i.filename.encode("utf-8")
            um = i.external_attr >> 16
            kind = um & 0o170000 if i.create_system == 3 else 0
            fl = (SYM if kind == 0o120000 else SPECIAL if kind in (0o020000, 0o060000, 0o010000, 0o140000) else 0) | (ENC if i.flag_bits & 1 else 0)
            if kind == 0o040000 or i.external_attr & 0x10:
                fl |= DIR
            isdir = fl & DIR or raw.endswith(b"/")
            mode = um & 0o7777 if i.create_system == 3 and um else (0o755 if isdir else 0o644)
            mt = calendar.timegm(i.date_time + (0, 0, 0))
            ents.append(want(raw, fl, i.file_size, i.compress_size, i.header_offset, mt, mode, i.compress_type, i.CRC))
    return lines(ents)


class Z:
    """A hand-made zip: members with explicit raw fields, optional prefix / zip64 end record / comment."""

    def __init__(self) -> None:
        self.body = bytearray()
        self.recs: list[bytes] = []
        self.n = 0
        self.exp: list[tuple] = []

    def add(self, name: bytes, data: bytes = b"", method: int = 0, flags: int = 0, host: int = 3, ext_attr: int = 0o100644 << 16, dt: tuple = (2023, 11, 14, 22, 13, 20),
            extra_cd: bytes = b"", extra_lh: bytes = b"", z64_cd: bool = False, z64_extra: bytes | None = None, crc: int | None = None, size: int | None = None, expect: tuple | None = None,
            lh: bool = True) -> int:
        off = len(self.body)
        crc = zlib.crc32(data) & 0xFFFFFFFF if crc is None else crc
        size = len(data) if size is None else size
        t, d = dos(dt)
        if lh:
            self.body += struct.pack("<IHHHHHIIIHH", 0x04034B50, 20, flags, method, t, d, crc, len(data), size, len(name), len(extra_lh)) + name + extra_lh + data
        c32, u32, o32 = (0xFFFFFFFF,) * 3 if z64_cd else (len(data), size, off)
        if z64_cd:
            extra_cd = (z64_extra if z64_extra is not None else struct.pack("<HHQQQ", 1, 24, size, len(data), off)) + extra_cd
        self.recs.append(struct.pack("<IHHHHHHIIIHHHHHII", 0x02014B50, host << 8 | 30, 20, flags, method, t, d, crc, c32, u32, len(name), len(extra_cd), 0, 0, 0, ext_attr, o32) + name + extra_cd)
        self.n += 1
        if expect is not None:
            self.exp.append(expect)
        return off

    def done(self, comment: bytes = b"", prefix: bytes = b"", zip64: bool = False, reverse: bool = False, disk: int = 0, count: int | None = None) -> bytes:
        cd_off = len(self.body)
        n = self.n if count is None else count
        cd = b"".join(reversed(self.recs) if reverse else self.recs)
        end = bytes(self.body) + cd
        tail = b""
        if zip64:
            z = len(end)
            tail += struct.pack("<IQHHIIQQQQ", 0x06064B50, 44, 45, 45, 0, 0, n, n, len(cd), cd_off) + struct.pack("<IIQI", 0x07064B50, 0, z, 1)
        e32 = (0xFFFF, 0xFFFFFFFF, 0xFFFFFFFF) if zip64 else (n, len(cd), cd_off)
        tail += struct.pack("<IHHHHIIH", 0x06054B50, disk, 0, e32[0], e32[0], e32[1], e32[2], len(comment)) + comment
        return prefix + end + tail


def test_corpus(exe: Path, work: Path) -> Path:
    fx = build_corpus(work / "corpus", many=2000)
    names = [n for n, f in fx.items() if f.fmt == "zip"]
    got = zparse(exe, [fx[n].path for n in names])
    for n, blk in zip(names, got):
        w = from_zipfile(fx[n].path)
        if n == "overlap.zip":                                               # two records, one local header: the second is flagged
            assert "ov=1" in blk[0], (n, blk[0])
            continue
        assert blk[0].startswith("Z rc=0 ") and f"n={len(w)} " in blk[0], (n, blk[0], len(w))
        assert blk[1:] == w, f"{n}: first difference " + str(next(((a, b) for a, b in zip(blk[1:], w) if a != b), "length"))
    print(f"[PASS] AZ1. {len(names)} fixture zips ({', '.join(names)}) equal Python's zipfile: names, kinds, sizes, methods, CRCs, offsets, mtimes, modes")
    return work / "corpus"


def crafted(work: Path) -> dict[str, tuple[bytes, list[str], str]]:
    out: dict[str, tuple[bytes, list[str], str]] = {}
    mt = calendar.timegm((2023, 11, 14, 22, 13, 20, 0, 0, 0))

    z = Z()
    off = z.add(b"big.bin", b"BIGDATA", 0, z64_cd=True, expect=None)
    z.exp.append(want(b"big.bin", 0, 7, 7, off, mt, 0o644, 0, zlib.crc32(b"BIGDATA") & 0xFFFFFFFF))
    off = z.add(b"two.bin", b"22", 0, z64_cd=True)
    z.exp.append(want(b"two.bin", 0, 2, 2, off, mt, 0o644, 0, zlib.crc32(b"22") & 0xFFFFFFFF))
    out["zip64"] = (z.done(zip64=True), lines(z.exp), "rc=0 n=2 dmg=0")

    z = Z()
    off = z.add(b"c.txt", b"c")
    z.exp.append(want(b"c.txt", 0, 1, 1, off, mt, 0o644, 0, zlib.crc32(b"c") & 0xFFFFFFFF))
    fake = b"x" * 40 + b"PK\x05\x06" + b"\0" * 18 + b"y" * 300
    out["fake_eocd_comment"] = (z.done(comment=fake), lines(z.exp), "rc=0 n=1 dmg=0")

    z = Z()
    ut = struct.pack("<HHBI", 0x5455, 5, 1, 1600000000)
    off = z.add(b"ut.txt", b"u", extra_cd=ut)
    z.exp.append(want(b"ut.txt", 0, 1, 1, off, 1600000000, 0o644, 0, zlib.crc32(b"u") & 0xFFFFFFFF))
    nm = b"plain.txt"
    uni = "ünïcode/ñ.txt".encode()
    good = struct.pack("<HHBI", 0x7075, 5 + len(uni), 1, zlib.crc32(nm) & 0xFFFFFFFF) + uni
    off = z.add(nm, b"p", extra_cd=good)
    z.exp.append(want(uni, 0, 1, 1, off, mt, 0o644, 0, zlib.crc32(b"p") & 0xFFFFFFFF))
    nm2 = b"keep.txt"
    bad = struct.pack("<HHBI", 0x7075, 5 + 3, 1, 12345) + b"bad"
    off = z.add(nm2, b"k", extra_cd=bad)
    z.exp.append(want(nm2, 0, 1, 1, off, mt, 0o644, 0, zlib.crc32(b"k") & 0xFFFFFFFF))
    aes = struct.pack("<HHHHBH", 0x9901, 7, 2, 0x4541, 3, 8)
    off = z.add(b"aes.bin", b"E", 99, flags=1, extra_cd=aes)
    z.exp.append(want(b"aes.bin", ENC, 1, 1, off, mt, 0o644, 99, zlib.crc32(b"E") & 0xFFFFFFFF))
    out["extras"] = (z.done(), lines(z.exp), "rc=0 n=4 dmg=0 skip=0 uns=0 enc=1")

    z = Z()
    for raw, kw, fl, mode in (
        (b"dosdir", dict(host=0, ext_attr=0x10), DIR, 0o755), (b"unixdir", dict(ext_attr=0o040755 << 16), DIR, 0o755),
        (b"link", dict(ext_attr=0o120777 << 16), SYM, 0o777), (b"fifo", dict(ext_attr=0o010644 << 16), SPECIAL, 0o644),
        (b"dosfile", dict(host=0, ext_attr=0x20), 0, 0o644), (b"../up", dict(), 0, 0o644), (b"/abs/x", dict(), 0, 0o644),
        (b"suid", dict(ext_attr=0o104755 << 16), 0, 0o4755), (b"zeroattr", dict(ext_attr=0), 0, 0o644),
    ):
        off = z.add(raw, b"d", **kw)
        z.exp.append(want(raw, fl, 0 if fl & DIR else 1, 0 if fl & DIR else 1, off, mt, mode, 0, zlib.crc32(b"d") & 0xFFFFFFFF))
    out["attrs"] = (z.done(), [], "rc=0 n=9 dmg=0")

    z = Z()
    off = z.add(b"caf\x82.txt", b"1")
    z.exp.append(want("café.txt".encode(), 0, 1, 1, off, mt, 0o644, 0, zlib.crc32(b"1") & 0xFFFFFFFF))
    off = z.add("plain-ü.txt".encode(), b"2")                           # UTF-8 bytes, no flag: kept as they are
    z.exp.append(want("plain-ü.txt".encode(), 0, 1, 1, off, mt, 0o644, 0, zlib.crc32(b"2") & 0xFFFFFFFF))
    off = z.add("flag-é.txt".encode(), b"3", flags=0x800)
    z.exp.append(want("flag-é.txt".encode(), 0, 1, 1, off, mt, 0o644, 0, zlib.crc32(b"3") & 0xFFFFFFFF))
    out["names"] = (z.done(), lines(z.exp), "rc=0 n=3 dmg=0")

    z = Z()
    crc = lambda b: zlib.crc32(b) & 0xFFFFFFFF
    for nm, dt in ((b"y1980", (1980, 1, 1, 0, 0, 0)), (b"leap", (2024, 2, 29, 23, 59, 58)), (b"far", (2099, 12, 31, 23, 59, 58)), (b"feb", (2023, 2, 28, 1, 2, 4))):
        off = z.add(nm, b"t", dt=dt)
        z.exp.append(want(nm, 0, 1, 1, off, calendar.timegm(dt + (0, 0, 0)), 0o644, 0, crc(b"t")))
    out["dates"] = (z.done(), lines(z.exp), "rc=0 n=4 dmg=0")

    z = Z()
    off = z.add(b"lie-stored", b"ab", 0, size=100)
    z.exp.append(want(b"lie-stored", 0, 2, 2, off, mt, 0o644, 0, crc(b"ab")))
    off = z.add(b"lie-deflate", b"ab", 8, size=100)
    z.exp.append(want(b"lie-deflate", 0, 100, 2, off, mt, 0o644, 8, crc(b"ab")))
    out["size lies"] = (z.done(), lines(z.exp), "rc=0 n=2 dmg=0")

    z = Z()
    # the ZIP64 field holds one value where three are due; the bytes right behind it read as a valid offset (0) if the length is not checked
    z.add(b"short64", b"s", z64_cd=True, z64_extra=struct.pack("<HHQ", 1, 8, 1) + struct.pack("<HH", 0x9999, 12) + bytes(4) + struct.pack("<Q", 0))
    off = z.add(b"fine64", b"f", z64_cd=True)
    z.exp.append(want(b"fine64", 0, 1, 1, off, mt, 0o644, 0, crc(b"f")))
    out["zip64 field too short"] = (z.done(zip64=True), lines(z.exp), "rc=0 n=1 dmg=1")

    z = Z()
    offs = [z.add(f"f{i}".encode(), b"x" * (10 + i)) for i in range(4)]
    for i, o in enumerate(offs):
        z.exp.append(want(f"f{i}".encode(), 0, 10 + i, 10 + i, o, mt, 0o644, 0, crc(b"x" * (10 + i))))
    out["reordered directory"] = (z.done(reverse=True), lines(z.exp), "rc=0 n=4 dmg=0 skip=0 uns=0 enc=0 ov=0")

    z = Z()
    off = z.add(b"j.txt", b"j")
    z.exp.append(want(b"j.txt", 0, 1, 1, off, mt, 0o644, 0, crc(b"j")))
    out["trailing junk"] = (z.done() + b"TRAILING-JUNK", lines(z.exp), "rc=0 n=1 dmg=0")

    z = Z()
    off = z.add(b"s.txt", b"s")
    z.exp.append(want(b"s.txt", 0, 1, 1, off + 2000, mt, 0o644, 0, zlib.crc32(b"s") & 0xFFFFFFFF))
    out["prefix"] = (z.done(prefix=b"MZ" + b"\x90" * 1998), lines(z.exp), "rc=0 n=1 dmg=0")
    return out


def test_crafted(exe: Path, work: Path) -> None:
    items = crafted(work)
    paths = []
    for n, (data, _, _) in items.items():
        p = work / f"crafted_{n}.zip"
        p.write_bytes(data)
        paths.append(p)
    got = zparse(exe, paths)
    for (n, (data, want_lines, head)), blk in zip(items.items(), got):
        assert blk[0].startswith("Z " + head), (n, blk[0])
        if n == "attrs":
            names = {bytes.fromhex(ln.split()[0]): ln.split() for ln in blk[1:]}
            checks = {b"dosdir/": (DIR, 0o755), b"unixdir/": (DIR, 0o755), b"link": (SYM, 0o777), b"fifo": (SPECIAL, 0o644), b"dosfile": (0, 0o644),
                      b"../up": (UNSAFE, 0o644), b"abs/x": (0, 0o644), b"suid": (0, 0o4755), b"zeroattr": (0, 0o644)}
            assert set(names) == set(checks), (set(names) ^ set(checks))
            for k, (fl, mode) in checks.items():
                assert int(names[k][1]) == fl and int(names[k][6]) == mode, (k, names[k], fl, mode)
            continue
        assert blk[1:] == want_lines, f"{n}:\n" + "\n".join(f"  got  {a}\n  want {b}" for a, b in zip(blk[1:], want_lines) if a != b)[:1500] + f"\n{len(blk) - 1} vs {len(want_lines)}"
    print(f"[PASS] AZ2. {len(items)} hand-made zips (ZIP64 records, fake end record in the comment, extras 0x5455 / 0x7075 / 0x9901, attributes, names, prefix)")


def test_damage(exe: Path, work: Path) -> None:
    cases: dict[str, tuple[bytes, str]] = {}
    z = Z()
    z.add(b"ok.txt", b"ok")
    z.add(b"nolh.txt", b"x", lh=False)
    cases["missing local header"] = (z.done(), "rc=0 n=1 dmg=1")
    z = Z()
    z.add(b"short.bin", b"ABCDEFGH")
    d = bytearray(z.done())
    c = d.index(b"PK\x01\x02")
    struct.pack_into("<I", d, c + 20, 100000)
    cases["comp size beyond the file"] = (bytes(d), "rc=0 n=1 dmg=1")
    z = Z()
    z.add(b"a", b"1")
    z.add(b"b", b"2")
    cases["count larger than the directory"] = (z.done(count=5), "rc=0 n=2 dmg=1")
    z = Z()
    z.add(b"a", b"1")
    cases["multi-disk"] = (z.done(disk=1), "rc=-95")
    cases["not a zip"] = (random.Random(5).randbytes(3000), "rc=-22")
    cases["tiny"] = (b"PK\x05\x06", "rc=-22")
    cases["empty file"] = (b"", "rc=-22")
    cases["eocd only"] = (Z().done(), "rc=0 n=0")
    z = Z()
    z.add(b"x", b"1" * 50)
    d = bytearray(z.done())
    c = d.index(b"PK\x01\x02")
    struct.pack_into("<I", d, c + 42, 0xFFFFFFFF)
    cases["offset beyond the file"] = (bytes(d), "rc=0 n=0 dmg=1")
    z = Z()
    z.add(b"huge-name", b"1")
    d = bytearray(z.done())
    struct.pack_into("<H", d, d.index(b"PK\x01\x02") + 28, 60000)
    cases["name longer than the directory"] = (bytes(d), "rc=0 n=0 dmg=1")
    paths = []
    for i, (n, (data, _)) in enumerate(cases.items()):
        p = work / f"zdamage{i}.zip"
        p.write_bytes(data)
        paths.append(p)
    for (n, (_, head)), blk in zip(cases.items(), zparse(exe, paths)):
        assert blk[0].startswith("Z " + head), (n, blk[0])
    ov = zparse(exe, [work / "corpus" / "overlap.zip"])[0]
    assert "ov=1" in ov[0], ov
    flags = [int(ln.split()[1]) for ln in ov[1:]]
    assert flags and all(f & UNSAFE for f in flags), ov
    print(f"[PASS] AZ3. {len(cases)} damaged / foreign inputs and the overlapping-members zip behave as specified")


def mutate(rnd: random.Random, data: bytes) -> bytes:
    b = bytearray(data)
    k = rnd.randrange(6)
    if k == 0 and b:
        for _ in range(rnd.randrange(1, 9)):
            b[rnd.randrange(len(b))] = rnd.randrange(256)
    elif k == 1:
        b = b[:rnd.randrange(len(b) + 1)]
    elif k == 2:
        e, c = b.rfind(b"PK\x05\x06"), b.find(b"PK\x01\x02")
        if e >= 0 and e + 20 <= len(b):
            struct.pack_into("<HHII", b, e + 8, *rnd.choice([(0xFFFF, 0xFFFF, 0xFFFFFFFF, 0xFFFFFFFF), (0, 0, 0, 0), (1, 1, 0xFFFFFFFF, 0), (0xFFFE, 0xFFFE, 5, len(b))]))
        if c >= 0 and c + 34 <= len(b) and rnd.random() < 0.7:
            struct.pack_into("<IIHHH", b, c + 20, rnd.choice([0xFFFFFFFF, 0, 1 << 31]), rnd.choice([0xFFFFFFFF, 0, 1 << 31]), rnd.randrange(300), rnd.randrange(300), rnd.randrange(300))
    elif k == 3 and b:
        for _ in range(rnd.randrange(1, 30)):
            b[rnd.randrange(len(b))] ^= 1 << rnd.randrange(8)
    elif k == 4:
        c = b.find(b"PK\x01\x02")
        if c >= 0 and c + 46 <= len(b):
            struct.pack_into("<I", b, c + 42, rnd.choice([0xFFFFFFFF, len(b), len(b) - 1, 0]))
    else:
        i = rnd.randrange(len(b) + 1)
        b[i:i + 400] = bytes(400)
    return bytes(b)


def test_fuzz(exe: Path, work: Path, seeds: list[Path]) -> None:
    rnd = random.Random(0x21F5)
    pool = [p.read_bytes() for p in seeds if p.stat().st_size < 400000]
    done, faults = 0, []
    for start in range(0, 1500, 50):
        batch = []
        for j in range(50):
            data = mutate(rnd, rnd.choice(pool))
            q = work / f"zfz_{j}.zip"
            q.write_bytes(data)
            batch.append((q, len(data)))
        for (q, n), blk in zip(batch, zparse(exe, [q for q, _ in batch])):
            done += 1
            if "fault" in blk[0]:
                faults.append((q.name, blk[0]))
                continue
            assert blk[0].split()[1] in ("rc=0", "rc=-22", "rc=-95"), blk[0]
            for ln in blk[1:]:
                f = ln.split()
                assert int(f[4]) <= n, f"local header offset outside the file ({n} B): {ln[:90]}"
    assert not faults, faults[:3]
    print(f"[PASS] AZ4. {done} mutants / forged fields: no fault, every local-header offset inside the file")


def test_aarch64(x86: Path, work: Path, paths: list[Path]) -> None:
    if not H.available("aarch64"):
        print("[SKIP] AZ5. aarch64 toolchain / qemu-user missing")
        return
    assert zparse(build(work / "a64", "aarch64"), paths, "aarch64") == zparse(x86, paths), "aarch64 and x86_64 disagree"
    print(f"[PASS] AZ5. {len(paths)} archives parse identically under qemu-aarch64")


# Equivalent, not listed: dropping "ex->aes = true;" (an AES member also carries method 99 and the encryption flag) and the locator test
# in z64_record (the record itself is found by its signature).
MUTANTS = [
    ("exact comment", "if (p + 22 + cl == len) return (int64_t)p;", "if (false) return (int64_t)p;"),
    ("loose end record", "if (loose < 0 && p + 22 + cl < len) loose = (int64_t)p;", ""),
    ("multi-disk", "if ((disk && disk != 0xFFFF) || (cd_disk && cd_disk != 0xFFFF)) return -EOPNOTSUPP;", ""),
    ("prefix shift", "z->shift = (int64_t)(z->start - cd_off);", "z->shift = 0;"),
    ("valid utf-8 kept", "(gp & 0x800) || utf8_valid(nm, n)", "(gp & 0x800)"),
    ("cp437 table", "0x00C7, 0x00FC, 0x00E9,", "0x00C7, 0x00FC, 0x00E8,"),
    ("civil date", "y -= m <= 2;", ""),
    ("extended time", "ex->mtime = load_le32(x + 1);", "ex->mtime = 0;"),
    ("unicode path crc", "load_le32(x + 1) == (~crc32_update(0xFFFFFFFFu, name, nlen))", "true"),
    ("zip64 field length", "if (sz - used < 8) return false;", ""),
    ("local header magic", "load_le32(d + lho) != SIG_LH", "false"),
    ("comp clamp", "ex.comp = len - lho - hdr;", ""),
    ("stored size clamp", "if (!method && ex.size > ex.comp) ex.size = ex.comp;", ""),
    ("overlap", "if (it->off < prev_end) {", "if (false) {"),
    ("sorted shortcut", "if (i && x->items[i - 1].off > x->items[i].off) sorted = false;", ""),
    ("dos dir bit", "if (ea & 0x10) fl |= ARC_F_DIR;", ""),
    ("special type", "fl |= ARC_F_SPECIAL;", ""),
    ("default mode", "if (!unix_attr) *mode = (fl & ARC_F_DIR) ? 0755 : 0644;", ""),
    ("encryption bit", "if ((gp & 1) || method == 99 || ex.aes) {", "if (method == 99 || ex.aes) {"),
    ("record bounds", "if (avail - 46 < nlen || avail - 46 - nlen < xlen || avail - 46 - nlen - xlen < clen) return 1;", ""),
    ("short directory", "if (parsed < z.total) info->damaged = 1;", ""),
]


def test_mutants(work: Path, paths: list[Path]) -> None:
    text = (ROOT / SRC).read_text()
    for name, old, _ in MUTANTS:
        assert old in text, f"mutation anchor vanished: {name}"
    base = zparse(build(work / "mbase"), paths)
    survivors: list[str] = []

    def job(i: int, name: str, old: str, new: str):
        def go() -> None:
            d = work / f"m{i}"
            d.mkdir()
            p = d / "arc_zip.c"
            p.write_text(text.replace(old, new, 1))
            try:
                exe = H.build("x86_64", d, [s if s != SRC else str(p) for s in SRCS], cflags=CFLAGS)
                same = zparse(exe, paths, timeout=30) == base
            except (subprocess.CalledProcessError, subprocess.TimeoutExpired, AssertionError):
                same = False
            if same:
                survivors.append(name)
        return go

    H.parallel([job(i, *m) for i, m in enumerate(MUTANTS)])
    assert not survivors, f"mutants not noticed: {sorted(survivors)}"
    print(f"[PASS] AZ6. {len(MUTANTS)}/{len(MUTANTS)} mutants of arc_zip.c are noticed")


def main() -> None:
    with H.workdir() as d:
        w = Path(d).resolve()
        exe = build(w / "x86")
        corpus = test_corpus(exe, w)
        test_crafted(exe, w)
        test_damage(exe, w)
        zips = sorted(corpus.glob("*.zip"))
        crafted_zips = sorted(w.glob("crafted_*.zip")) + sorted(w.glob("zdamage*.zip"))
        test_fuzz(exe, w, zips + crafted_zips)
        small = [p for p in zips if p.stat().st_size < 400000] + crafted_zips
        test_aarch64(exe, w, small)
        test_mutants(w, small)
    print("ALL C ARC ZIP TESTS PASSED.")


if __name__ == "__main__":
    sys.exit(main())
