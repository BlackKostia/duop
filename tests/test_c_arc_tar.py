#!/usr/bin/env python3
"""src/fs/arc_tar.c: the tar parser of the archive layer (docs/ARCHIVE_TOOLS_PLAN.md, phase 1) - ustar prefix, GNU long names (L / K), PAX
extended headers (path, linkpath, size, mtime), base-256 numbers, links, special files, header checksums, truncation.

AT1 the fixture corpus (PAX, GNU, ustar, links, base-256, empty, 2000 members): names, kinds, sizes, data offsets, mtimes, modes, link
    targets equal what Python's tarfile reads
AT2 hand-made archives: ustar prefix, GNU magic ignoring the prefix field, a 100-byte name, hardlink / directory headers whose size field
    must not swallow the next header, PAX size / path / mtime, GNU L / K, sparse and vendor types, signed-checksum headers, '..' and '/' names
AT3 damage: a bad checksum / truncated data / truncated header / bogus PAX length stop the parse with the earlier members kept (damaged = 1);
    a first header that is not a tar header is -EINVAL; junk behind the end blocks is ignored
AT4 1500 mutants and forged size fields of the corpus, read from the end of a page with a guard page behind: never a fault, never a member
    reaching outside the data
AT5 aarch64 (qemu-user) equals x86_64;  AT6 mutation check
"""

import io
import random
import struct
import subprocess
import sys
import tarfile
import tempfile
from pathlib import Path

import c_harness as H
from archive_fixtures import MTIME, build_corpus
from test_c_arc_index import DIR, normalize

ROOT = H.ROOT
SRC = "src/fs/arc_tar.c"
SRCS = ["tests/harness/arcparse_cli.c", SRC, "src/fs/arc_index.c", "src/core/hash.c", "src/core/string.c", "src/core/utf8.c",
        "src/core/memory.c", "src/core/syscall.c", "src/core/try_rt.c", "src/core/mem_prims.c"]
SYM, HARD, UNSAFE, SPECIAL = 2, 4, 16, 32
EINVAL = -22


def build(work: Path, arch: str = "x86_64", srcs: list[str] | None = None) -> Path:
    work.mkdir(parents=True, exist_ok=True)
    return H.build(arch, work, srcs or SRCS, cflags=["-I", str(ROOT / "tests/harness"), "-DWITH_TAR"])


def parse(exe: Path, paths: list[Path], arch: str = "x86_64", kind: str = "T", timeout: int = 120) -> list[list[str]]:
    """One block of output lines per file: [header, entry, entry, ...]."""
    res = H.run(arch, exe, [], "".join(f"{kind} {p}\n" for p in paths).encode(), timeout=timeout)
    assert res.returncode == 0, res.stderr[-300:]
    blocks: list[list[str]] = []
    for line in res.stdout.decode().splitlines():
        if line[:2] in (f"{kind} ",):
            blocks.append([line])
        else:
            blocks[-1].append(line)
    assert len(blocks) == len(paths), (len(blocks), len(paths))
    return blocks


def want_line(raw: bytes, flags: int = 0, size: int = 0, off: int = 0, mtime: int = MTIME, mode: int = 0o644, link: bytes | None = None) -> tuple | None:
    name, unsafe, err = normalize(raw, flags)
    if name is None:
        return None
    fl = flags | (DIR if name.endswith(b"/") else 0) | (UNSAFE if unsafe else 0)
    return (name, f"{name.hex()} {fl} {size} {size} {off} {mtime} {mode} 0 0 {link.hex() if link is not None else '-'}")


def lines(entries: list[tuple | None]) -> list[str]:
    last = {e[0]: e[1] for e in entries if e}
    return [last[k] for k in sorted(last)]


def expect_tarfile(data: bytes) -> list[str]:
    ents = []
    with tarfile.open(fileobj=io.BytesIO(data)) as tf:
        for ti in tf:
            raw = ti.name.encode("utf-8", "surrogateescape") + (b"/" if ti.isdir() else b"")
            fl = SYM if ti.issym() else HARD if ti.islnk() else SPECIAL if (ti.isfifo() or ti.ischr() or ti.isblk()) else 0
            link = ti.linkname.encode("utf-8", "surrogateescape") if (ti.issym() or ti.islnk()) else None
            ents.append(want_line(raw, fl, ti.size if ti.isreg() else 0, ti.offset_data, int(ti.mtime), ti.mode & 0o7777, link))
    return lines(ents)


def hdr(name: bytes = b"", size: int = 0, tf: bytes = b"0", link: bytes = b"", prefix: bytes = b"", magic: bytes = b"ustar\x0000", mode: int = 0o644,
        mtime: int = MTIME, signed: bool = False, sizefield: bytes | None = None) -> bytes:
    h = bytearray(512)
    h[0:len(name[:100])] = name[:100]
    h[100:108] = b"%07o\0" % mode
    h[108:124] = b"0000000\0" * 2
    h[124:136] = sizefield if sizefield is not None else b"%011o\0" % size
    h[136:148] = b"%011o\0" % mtime
    h[148:156] = b" " * 8
    h[156:157] = tf
    h[157:157 + len(link[:100])] = link[:100]
    h[257:265] = magic
    h[345:345 + len(prefix)] = prefix
    s = sum((b - 256 if b > 127 else b) for b in h) if signed else sum(h)
    h[148:156] = b"%06o\0 " % s
    return bytes(h)


def pax(recs: dict[bytes, bytes]) -> bytes:
    out = b""
    for k, v in recs.items():
        rest = b" " + k + b"=" + v + b"\n"
        d = len(str(len(rest)))
        if len(str(len(rest) + d)) != d:
            d += 1
        out += str(len(rest) + d).encode() + rest
    return out


class Tar:
    def __init__(self) -> None:
        self.buf = bytearray()

    def add(self, header: bytes, data: bytes = b"", pad: bool = True) -> int:
        """Appends header + data; returns the offset of the data."""
        off = len(self.buf) + 512
        self.buf += header + data + (b"\0" * (-len(data) % 512) if pad else b"")
        return off

    def done(self, end: bool = True) -> bytes:
        return bytes(self.buf) + (b"\0" * 1024 if end else b"")


def crafted() -> dict[str, tuple[bytes, list[str]]]:
    out: dict[str, tuple[bytes, list[str]]] = {}
    t, e = Tar(), []
    o = t.add(hdr(b"file.txt", 4, prefix=b"some/long/dir"), b"data")
    e.append(want_line(b"some/long/dir/file.txt", size=4, off=o))
    o = t.add(hdr(b"g.txt", 1, magic=b"ustar  \0", prefix=b"IGNORED-atime"), b"g")
    e.append(want_line(b"g.txt", size=1, off=o))
    o = t.add(hdr(b"n" * 100, 2), b"nn")
    e.append(want_line(b"n" * 100, size=2, off=o))
    o = t.add(hdr(b"hl", 123, tf=b"1", link=b"file.txt"))                 # a hard link with a size field: no data follows
    e.append(want_line(b"hl", HARD, 0, o, link=b"file.txt"))
    o = t.add(hdr(b"d/", 512, tf=b"5", mode=0o755))                       # a directory with a size field: its data is not skipped
    e.append(want_line(b"d/", 0, 0, o, mode=0o755))
    o = t.add(hdr(b"after", 3), b"abc")
    e.append(want_line(b"after", size=3, off=o))
    out["basic"] = (t.done(), lines(e))
    t, e = Tar(), []
    p = pax({b"path": "pax/üñï/long".encode(), b"mtime": b"1700000123.987654", b"linkpath": "l/ü".encode()})
    t.add(hdr(b"./PaxHeaders/x", len(p), tf=b"x"), p)
    o = t.add(hdr(b"short", 3, tf=b"2", link=b"ignored"))                 # a symlink: its size field is not data
    e.append(want_line("pax/üñï/long".encode(), SYM, 0, o, 1700000123, link="l/ü".encode()))
    p = pax({b"size": b"700"})
    t.add(hdr(b"./PaxHeaders/y", len(p), tf=b"x"), p)
    o = t.add(hdr(b"sized.bin", 0), b"Q" * 700)                           # the PAX size, not the header's 0, gives the data length
    e.append(want_line(b"sized.bin", size=700, off=o))
    o = t.add(hdr(b"next", 1), b"n")
    e.append(want_line(b"next", size=1, off=o))
    p = pax({b"comment": b"c"})
    t.add(hdr(b"./PaxHeaders/g", len(p), tf=b"g"), p)
    o = t.add(hdr(b"afterglobal", 1), b"z")
    e.append(want_line(b"afterglobal", size=1, off=o))
    out["pax"] = (t.done(), lines(e))
    t, e = Tar(), []
    name = b"gnu/" + b"L" * 150
    t.add(hdr(b"././@LongLink", len(name) + 1, tf=b"L", magic=b"ustar  \0"), name + b"\0")
    o = t.add(hdr(b"truncated-name", 5, magic=b"ustar  \0"), b"hello")
    e.append(want_line(name, size=5, off=o))
    tgt = b"target/" + b"K" * 120
    t.add(hdr(b"././@LongLink", len(tgt) + 1, tf=b"K", magic=b"ustar  \0"), tgt + b"\0")
    o = t.add(hdr(b"sym", 0, tf=b"2", link=b"short", magic=b"ustar  \0"))
    e.append(want_line(b"sym", SYM, 0, o, link=tgt))
    out["gnu"] = (t.done(), lines(e))
    t, e = Tar(), []
    o = t.add(hdr(b"sparse.dat", 1024, tf=b"S", magic=b"ustar  \0"), b"S" * 1024)
    e.append(want_line(b"sparse.dat", SPECIAL, 1024, o))
    t.add(hdr(b"vol", 0, tf=b"V"))
    o = t.add(hdr(b"dump", 5, tf=b"D"), b"dumpd")
    o = t.add(hdr(b"contig", 2, tf=b"7"), b"cc")
    e.append(want_line(b"contig", size=2, off=o))
    o = t.add(hdr(b"fifo", 0, tf=b"6"))
    e.append(want_line(b"fifo", SPECIAL, 0, o))
    o = t.add(hdr("é.bin".encode("latin-1"), 1, signed=True), b"s")
    e.append(want_line("é".encode("latin-1") + b".bin", size=1, off=o))
    o = t.add(hdr(b"bare", 0, tf=b"5", mode=0o755))                       # a directory whose name has no trailing slash
    e.append(want_line(b"bare", DIR, 0, o, mode=0o755))
    o = t.add(hdr(b"../up/x", 1), b"u")
    e.append(want_line(b"../up/x", size=1, off=o))
    o = t.add(hdr(b"/abs/path", 1), b"a")
    e.append(want_line(b"/abs/path", size=1, off=o))
    o = t.add(hdr(b"b256", 0, sizefield=b"\x80" + (77).to_bytes(11, "big")), b"B" * 77)
    e.append(want_line(b"b256", size=77, off=o))
    out["misc"] = (t.done(), lines(e))
    return out


def test_corpus(exe: Path, work: Path) -> None:
    fx = build_corpus(work / "corpus", many=2000)
    names = [n for n, f in fx.items() if f.fmt == "tar"]
    got = parse(exe, [fx[n].path for n in names])
    for n, blk in zip(names, got):
        want = expect_tarfile(fx[n].path.read_bytes())
        assert blk[0].startswith("T rc=0 ") and f"n={len(want)} dmg=0" in blk[0], (n, blk[0], len(want))
        assert blk[1:] == want, f"{n}: first difference " + str(next(((a, b) for a, b in zip(blk[1:], want) if a != b), "length")) 
    print(f"[PASS] AT1. {len(names)} fixture tars ({', '.join(names)}) equal Python's tarfile: names, kinds, sizes, offsets, mtimes, modes, links")


def test_crafted(exe: Path, work: Path) -> dict[str, Path]:
    items = crafted()
    paths = {}
    for n, (data, _) in items.items():
        p = work / f"crafted_{n}.tar"
        p.write_bytes(data)
        paths[n] = p
    got = parse(exe, list(paths.values()))
    for (n, (_, want)), blk in zip(items.items(), got):
        assert blk[0].startswith("T rc=0 "), (n, blk[0])
        assert blk[1:] == want, f"{n}:\n" + "\n".join(f"  got  {a}\n  want {b}" for a, b in zip(blk[1:], want) if a != b)[:1200] + f"\n{len(blk) - 1} vs {len(want)}"
    unsupported = [b[0] for (n, _), b in zip(items.items(), got) if n == "misc"][0]
    assert "uns=1" in unsupported, unsupported
    print(f"[PASS] AT2. {len(items)} hand-made tars (prefix, GNU magic, long names, PAX size / path / mtime, sizes of links and directories, special and vendor types, signed checksums, base-256)")
    return paths


def test_damage(exe: Path, work: Path) -> None:
    t = Tar()
    t.add(hdr(b"one", 3000), b"1" * 3000)
    t.add(hdr(b"two", 600), b"2" * 600)
    good = t.done()
    second = 512 + 3072
    cases: dict[str, tuple[bytes, str, int, int]] = {}                      # name: (data, rc, n, damaged)
    bad = bytearray(good)
    bad[second + 5] ^= 0xFF
    cases["bad checksum of 2nd header"] = (bytes(bad), "0", 1, 1)
    cases["first header garbage"] = (b"x" * 4096, "-22", 0, 0)
    cases["random bytes"] = (random.Random(3).randbytes(5000), "-22", 0, 0)
    cases["data cut"] = (good[:512 + 300], "0", 1, 1)
    cases["header cut"] = (good[:second + 200], "0", 1, 1)
    cases["zero blocks then junk"] = (good + b"JUNK" * 500, "0", 2, 0)
    cases["no end blocks"] = (good[:-1024], "0", 2, 0)
    cases["tiny"] = (b"abc", "-22", 0, 0)
    cases["empty file"] = (b"", "0", 0, 0)
    p = pax({b"path": b"x"})
    t = Tar()
    t.add(hdr(b"ok", 1), b"o")
    t.add(hdr(b"./PaxHeaders/z", len(p), tf=b"x"), b"99999 path=" + b"x" * (len(p) - 11))
    t.add(hdr(b"never", 1), b"n")
    cases["bogus pax length"] = (t.done(), "0", 1, 1)
    t = Tar()
    t.add(hdr(b"ok", 1), b"o")
    t.add(hdr(b"neg", 0, sizefield=b"\xff" * 12))
    t.add(hdr(b"never", 1), b"n")
    cases["negative base-256 size"] = (t.done(), "0", 1, 1)
    paths = []
    for i, (n, (data, *_)) in enumerate(cases.items()):
        p = work / f"damage{i}.tar"
        p.write_bytes(data)
        paths.append(p)
    for (n, (_, rc, cnt, dmg)), blk in zip(cases.items(), parse(exe, paths)):
        assert blk[0].startswith(f"T rc={rc} n={cnt} dmg={dmg} "), (n, blk[0])
    print(f"[PASS] AT3. {len(cases)} damaged / foreign inputs: earlier members kept and damage reported, non-tars refused with -EINVAL")


def mutants_of(rnd: random.Random, data: bytes) -> bytes:
    b = bytearray(data)
    k = rnd.randrange(6)
    if k == 0 and b:
        for _ in range(rnd.randrange(1, 9)):
            b[rnd.randrange(len(b))] = rnd.randrange(256)
    elif k == 1:
        b = b[:rnd.randrange(len(b) + 1)]
    elif k == 2 and len(b) >= 512:
        pos = 512 * rnd.randrange(len(b) // 512)
        b[pos + 124:pos + 136] = rnd.choice([b"77777777777\0", b"\x80" + (1 << 62).to_bytes(11, "big"), b"\xff" * 12, b"\x80" + (2**63 - 1).to_bytes(11, "big"), b" " * 12])
    elif k == 3 and b:
        for _ in range(rnd.randrange(1, 30)):
            b[rnd.randrange(len(b))] ^= 1 << rnd.randrange(8)
    elif k == 4 and len(b) >= 512:
        pos = 512 * rnd.randrange(len(b) // 512)
        b[pos + 156] = rnd.choice(b"xgLKS0123456789DVMN")
    else:
        i = rnd.randrange(len(b) + 1)
        b[i:i + 600] = bytes(600)
    return bytes(b)


def test_fuzz(exe: Path, work: Path, srcs: list[Path]) -> None:
    rnd = random.Random(0xF7A5)
    seeds = [p.read_bytes() for p in srcs if p.stat().st_size < 400000]
    done = 0
    faults = []
    for start in range(0, 1500, 50):
        batch = []
        for j in range(50):
            data = mutants_of(rnd, rnd.choice(seeds))
            q = work / f"fz_{j}.tar"
            q.write_bytes(data)
            batch.append((q, len(data)))
        blocks = parse(exe, [q for q, _ in batch])
        for (q, n), blk in zip(batch, blocks):
            done += 1
            if "fault" in blk[0]:
                faults.append((q.name, blk[0]))
                continue
            assert blk[0].split()[1] in ("rc=0", "rc=-22"), blk[0]
            for ln in blk[1:]:
                f = ln.split()
                size, off, flags = int(f[2]), int(f[4]), int(f[1])
                assert off <= n and (flags & 7 or off + size <= n), f"member outside the data ({n} B): {ln[:80]}"
    assert not faults, faults[:3]
    print(f"[PASS] AT4. {done} mutants / forged size fields: no fault, no member outside the data")


def test_aarch64(x86: Path, work: Path, paths: list[Path]) -> None:
    if not H.available("aarch64"):
        print("[SKIP] AT5. aarch64 toolchain / qemu-user missing")
        return
    assert parse(build(work / "a64", "aarch64"), paths, "aarch64") == parse(x86, paths), "aarch64 and x86_64 disagree"
    print(f"[PASS] AT5. {len(paths)} archives parse identically under qemu-aarch64")


MUTANTS = [
    ("ustar prefix", "if (posix && h[345]) {", "if (false && h[345]) {"),
    ("gnu magic", "h[262] == 0", "true"),
    ("checksum", "if (!tar_checksum_ok(h)) {", "if (false) {"),
    ("signed checksum", " || (int64_t)want == sum_s", ""),
    ("base256", "if (p[0] & 0x80) {", "if (false) {"),
    ("link size zero", "if (type >= '1' && type <= '6') size = 0;", ""),
    ("size clamp", "size = avail;", ""),
    ("pax size", "ext->has_size = true;", "ext->has_size = false;"),
    ("pax mtime", "ext->has_mtime = true;", "ext->has_mtime = false;"),
    ("gnu longname", "case 'L':", "case 'Z':"),
    ("sparse counted", "info->unsupported++;", ""),
    ("dir flag", "if (type == '5') pr.flags |= ARC_F_DIR;", ""),
    ("padding of meta", "uint64_t padded = (size + 511) & ~(uint64_t)511;", "uint64_t padded = size;"),
    ("padding of entry", "ext = (Ext){0};\n        uint64_t padded = (size + 511) & ~(uint64_t)511;", "ext = (Ext){0};\n        uint64_t padded = size;"),
    ("pax len bound", "if (rl < 4 || rl > rem) return false;", "if (rl < 4) return false;"),
    ("end block", "if (bytes_zero(h, 512)) break;", ""),
    ("first header", "if (first) return -EINVAL;", ""),
    ("torn header", "                info->damaged = 1;\n            }\n            break;", "            }\n            break;"),
    ("stub is no tar", "if (first) return -EINVAL;\n                info->damaged = 1;\n            }", "info->damaged = 1;\n            }"),
    ("ext reset", "ext = (Ext){0};\n        uint64_t", "uint64_t"),
    ("vendor skipped", "if (type != 'V' && type != 'D' && type != 'M' && type != 'N') {", "if (true) {"),
]


def test_mutants(work: Path, paths: list[Path]) -> None:
    text = (ROOT / SRC).read_text()
    for name, old, _ in MUTANTS:
        assert old in text, f"mutation anchor vanished: {name}"
    base = parse(build(work / "mbase"), paths)
    survivors: list[str] = []

    def job(i: int, name: str, old: str, new: str):
        def go() -> None:
            d = work / f"m{i}"
            d.mkdir()
            p = d / "arc_tar.c"
            p.write_text(text.replace(old, new, 1))
            try:
                exe = H.build("x86_64", d, [s if s != SRC else str(p) for s in SRCS], cflags=["-I", str(ROOT / "tests/harness"), "-DWITH_TAR"])
                same = parse(exe, paths, timeout=30) == base
            except (subprocess.CalledProcessError, subprocess.TimeoutExpired, AssertionError):
                same = False
            if same:
                survivors.append(name)
        return go

    H.parallel([job(i, *m) for i, m in enumerate(MUTANTS)])
    assert not survivors, f"mutants not noticed: {sorted(survivors)}"
    print(f"[PASS] AT6. {len(MUTANTS)}/{len(MUTANTS)} mutants of arc_tar.c are noticed")


def main() -> None:
    with H.workdir() as d:
        w = Path(d).resolve()
        exe = build(w / "x86")
        test_corpus(exe, w)
        crafted_paths = test_crafted(exe, w)
        test_damage(exe, w)
        corpus_tars = [p for p in (w / "corpus").glob("*.tar")]
        test_fuzz(exe, w, list(crafted_paths.values()) + corpus_tars)
        both = list(crafted_paths.values()) + [p for p in corpus_tars if p.stat().st_size < 400000]
        test_aarch64(exe, w, both)
        test_mutants(w, both + [w / f"damage{i}.tar" for i in range(11)])
    print("ALL C ARC TAR TESTS PASSED.")


if __name__ == "__main__":
    sys.exit(main())
