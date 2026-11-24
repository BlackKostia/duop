#!/usr/bin/env python3
"""bzip2 in the archive layer (src/fs/arc_detect.c, arc_source.c, arc_extract.c on src/fs/bzip2_dec.c; docs/ARCHIVE_ROADMAP_2026-11-24.md, phase 4).

BA1 a plain .bz2 is a one-member archive, a .tar.bz2 / .tbz2 / .tbz (or a tar.bz2 under any name, even when its first block is big) a tar:
    names, exact bytes, several streams and blocks, an empty payload; a bad CRC / cut / opaque file is not entered (r=4)
BA2 zip members compressed with method 12 (bzip2) list, extract to their exact bytes and fail on a wrong CRC
BA3 unpacking and testing: .bz2, .tar.bz2, a renamed one, a zip with a bzip2 member; damaged input leaves nothing
BA6 the synchronous open caps what a codec may unpack (a quarter for bzip2, half for xz)
BA4 aarch64 (qemu-user) gives the x86_64 results;  BA5 mutation check
"""

import gzip
import os
import random
import re
import struct
import subprocess
import sys
import zlib
from pathlib import Path

import arcunp_support as US
import c_harness as H
import test_c_arc_vfs as V
from archive_fixtures import Expect, tar_bytes
from test_c_arc_detect import ENTRIES, ROOT, TEXT, kids
from bz_support import bz


def zip_with_methods(members: list[tuple[str, bytes, int]]) -> bytes:
    """A zip whose members are stored (0) or bzip2 (12) compressed."""
    out, central = bytearray(), bytearray()
    for name, data, method in members:
        comp = bz(data) if method == 12 else data
        off, nm, crc = len(out), name.encode(), zlib.crc32(data)
        out += struct.pack("<IHHHHHIIIHH", 0x04034B50, 63, 0, method, 0, 0x21, crc, len(comp), len(data), len(nm), 0) + nm + comp
        central += struct.pack("<IHHHHHHIIIHHHHHII", 0x02014B50, 63, 63, 0, method, 0, 0x21, crc, len(comp), len(data), len(nm), 0, 0, 0, 0, 0, off) + nm
    cd = len(out)
    return bytes(out + central + struct.pack("<IHHHHIIH", 0x06054B50, 0, 0, len(members), len(members), len(central), cd, 0))


def make_fixtures(d: Path) -> None:
    d.mkdir()
    rnd = random.Random(3)
    tar = tar_bytes(ENTRIES)
    big_tar = tar_bytes([Expect("blob.bin", data=rnd.randbytes(400000)), Expect("b.txt", data=b"BBB")])      # the first bzip2 block is big
    (d / "notes.txt.bz2").write_bytes(bz(TEXT))
    (d / "blob.dat").write_bytes(bz(rnd.randbytes(30000)))
    (d / "multi.bz2").write_bytes(bz(b"first part\n") + bz(b"second part\n"))
    (d / "blocks.bz2").write_bytes(bz(TEXT * 40, 1))
    (d / "empty.bz2").write_bytes(bz(b""))
    (d / "a.tar.bz2").write_bytes(bz(tar))
    (d / "b.tbz2").write_bytes(bz(tar))
    (d / "tar_as.dat").write_bytes(bz(tar))
    (d / "tar_in.bz2").write_bytes(bz(tar))                                                                   # a tar under a plain .bz2 name: still a tar
    (d / "big.dat").write_bytes(bz(big_tar))
    (d / "zeros.tbz2").write_bytes(bz(bytes(1024)))                                                           # a tar of no members: the name settles it
    bad = bytearray(bz(TEXT))
    bad[20] ^= 0x10
    (d / "badsum.bz2").write_bytes(bytes(bad))
    (d / "trunc.bz2").write_bytes(bz(rnd.randbytes(20000))[:6000])
    (d / "opaque.bz2").write_bytes(b"opaque " * 200)
    (d / "empty0.bz2").write_bytes(b"")


def test_plain_and_tar(exe: Path, work: Path) -> None:
    d = work / "z"
    make_fixtures(d)
    singles = {"notes.txt.bz2": ("notes.txt", TEXT), "blob.dat": ("blob.dat.out", None), "multi.bz2": ("multi", b"first part\nsecond part\n"),
               "blocks.bz2": ("blocks", TEXT * 40), "empty.bz2": ("empty", b"")}
    tars = ("a.tar.bz2", "b.tbz2", "tar_as.dat", "tar_in.bz2")
    names = sorted(os.listdir(d))
    script = f"N {d}\n"
    for n in names:
        script += f"E {n}\n" + (f"X {singles[n][0]}\n" if n in singles else "X a.txt\n" if n in tars else "X b.txt\n" if n == "big.dat" else "") + "U\n"
    out = V.run(exe, script)[1:]
    i = 0
    for n in names:
        e = out[i]
        if n in singles:
            m, data = singles[n]
            size = len(data) if data is not None else 30000
            assert e.startswith("E r=1 ") and kids(e) == {m.encode(): ("f", size)}, (n, e[:140])
            assert out[i + 1].startswith("X r=0 len=%d " % size) and (data is None or out[i + 1].endswith("crc=%d" % zlib.crc32(data))), (n, out[i + 1])
            i += 3
        elif n in tars:
            assert e.startswith("E r=1 ") and kids(e) == {b"a.txt": ("f", 3), b"sub": ("d", 0)}, (n, e[:140])
            assert out[i + 1] == f"X r=0 len=3 crc={zlib.crc32(b'AAA')}", (n, out[i + 1])
            i += 3
        elif n == "zeros.tbz2":
            assert e.startswith("E r=1 ") and kids(e) == {}, (n, e[:140])
            i += 2
        elif n == "big.dat":
            assert e.startswith("E r=1 ") and kids(e) == {b"blob.bin": ("f", 400000), b"b.txt": ("f", 3)}, (n, e[:140])
            assert out[i + 1] == f"X r=0 len=3 crc={zlib.crc32(b'BBB')}", (n, out[i + 1])
            i += 3
        else:
            assert e.startswith("E r=4 ") and " vm=0 " in e, (n, e[:100])
            i += 2
    print(f"[PASS] BA1. .bz2, .tar.bz2, .tbz2 and bzip2 under any name: {len(singles)} one-member archives, {len(tars) + 1} tars; damaged files are r=4")


def test_zip12(exe: Path, work: Path) -> None:
    d = work / "zip"
    d.mkdir()
    data = {"a.txt": TEXT, "dir/b.bin": random.Random(8).randbytes(70000), "empty": b""}
    (d / "z12.zip").write_bytes(zip_with_methods([(n, b, 12 if b else 0) for n, b in data.items()]))
    z = bytearray(zip_with_methods([("c.txt", TEXT, 12)]))
    z[z.index(b"c.txt") + 5 + 60] ^= 0xFF                                           # a byte inside the bzip2 data
    (d / "bad12.zip").write_bytes(bytes(z))
    out = V.run(exe, f"N {d}\nE z12.zip\nX a.txt\nE dir\nX b.bin\nU\nU\nE bad12.zip\nX c.txt\n")
    assert out[1].startswith("E r=1 "), out[1][:100]
    assert out[2] == f"X r=0 len={len(TEXT)} crc={zlib.crc32(TEXT)}", out[2]
    assert out[4] == f"X r=0 len=70000 crc={zlib.crc32(data['dir/b.bin'])}", out[4]
    assert out[7].startswith("E r=1 ") and re.match(r"X r=-(22|74) ", out[8] + " "), out[7:9]
    print("[PASS] BA2. zip members with method 12 list and extract to their exact bytes; a damaged one fails (-EINVAL / -EBADMSG)")


def test_unpack(work: Path) -> None:
    d = work / "unp"
    d.mkdir()
    tar = tar_bytes(ENTRIES)
    (d / "notes.txt.bz2").write_bytes(bz(TEXT))
    (d / "a.tar.bz2").write_bytes(bz(tar))
    (d / "renamed.dat").write_bytes(bz(tar))
    (d / "z12.zip").write_bytes(zip_with_methods([("a.txt", TEXT, 12), ("b.txt", b"BBB", 0)]))
    bad = bytearray(bz(TEXT))
    bad[20] ^= 0x10
    (d / "bad.bz2").write_bytes(bytes(bad))
    exe = US.build(work / "ux86")
    script = ""
    for n in ("notes.txt.bz2", "a.tar.bz2", "renamed.dat", "z12.zip", "bad.bz2"):
        (work / "o" / n).mkdir(parents=True)
        script += US.U(d / n, work / "o" / n, smart=1)
    script += f"T {d / 'notes.txt.bz2'}\nT {d / 'a.tar.bz2'}\nT {d / 'bad.bz2'}\nT {d / 'z12.zip'}\n"
    out = US.run(exe, script)
    o = work / "o"
    r = US.kv(out[0])
    assert r["rc"] == 0 and r["files"] == 1 and r["dirs"] == 0 and (o / "notes.txt.bz2" / "notes.txt").read_bytes() == TEXT, out[0]
    for k, n in ((1, "a.tar.bz2"), (2, "renamed.dat")):
        assert US.kv(out[k])["rc"] == 0, out[k]
        assert [p.read_bytes() for p in sorted((o / n).rglob("*.txt"))] == [b"AAA", b"BBB"], (n, sorted(str(p) for p in (o / n).rglob("*")))
    assert US.kv(out[3])["rc"] == 0 and next((o / "z12.zip").rglob("a.txt")).read_bytes() == TEXT and next((o / "z12.zip").rglob("b.txt")).read_bytes() == b"BBB", out[3]
    assert US.kv(out[4])["rc"] != 0 and os.listdir(o / "bad.bz2") == [], f"a bad bzip2 must leave nothing: {out[4]}"
    t = [US.kv(x) for x in out[5:9]]
    assert t[0]["rc"] == 0 and t[0]["bytes"] == len(TEXT) and t[1]["rc"] == 0 and t[1]["files"] == 2, out[5:7]
    assert t[2]["rc"] != 0 and t[3]["rc"] == 0 and t[3]["files"] == 2, out[7:9]
    print("[PASS] BA3. .bz2, .tar.bz2 (also renamed) and a zip with a bzip2 member unpack and test; a bad .bz2 leaves nothing")


def test_aarch64(x86: Path, work: Path) -> None:
    if not H.available("aarch64"):
        print("[SKIP] BA4. aarch64 toolchain / qemu-user missing")
        return
    d = work / "a64"
    d.mkdir()
    (d / "n.txt.bz2").write_bytes(bz(TEXT))
    (d / "t.bz2").write_bytes(bz(tar_bytes(ENTRIES)))
    (d / "z.zip").write_bytes(zip_with_methods([("a.txt", TEXT, 12)]))
    script = f"N {d}\n" + "".join(f"E {n}\nL\nX {m}\nU\n" for n, m in (("n.txt.bz2", "n.txt"), ("t.bz2", "a.txt"), ("z.zip", "a.txt")))
    a, b = V.run(V.build(work / "a64b", "aarch64"), script, "aarch64"), V.run(x86, script)
    assert a == b, "aarch64 and x86_64 disagree"
    print("[PASS] BA4. bzip2 archives give the same output under qemu-aarch64")


MUTANTS = [
    ("bzip2 magic", "h[0] == 'B' && h[1] == 'Z' && h[2] == 'h' && h[3] >= '1' && h[3] <= '9'", "h[0] == 'B' && h[1] == 'Y' && h[2] == 'h' && h[3] >= '1' && h[3] <= '9'"),
    ("empty stream magic", "starts(h + 4, n - 4, bz_end, 6)", "false"),
    ("first block peek", "#define BZ_PEEK ((1u << 20) + (128u << 10))", "#define BZ_PEEK (64u << 10)"),
    ("tbz2 name", 'ends_ci(path, ".tbz2")', 'ends_ci(path, ".tb")'),
    ("bz2 name", 'if (ends_ci(path, ".bz2")) return ARC_BZ;', ""),
    ("single member name", 'static const char *const sfx[] = {".gz", ".zst", ".xz", ".bz2", ".tgz", ".tzst", ".txz", ".tbz2", ".tbz"};', 'static const char *const sfx[] = {".gz", ".zst", ".xz", ".tgz", ".tzst", ".txz", ".tbz2", ".tbz"};'),
    ("bzip2 decoder", "if (type == ARC_TBZ || type == ARC_BZ) return bzip2_decode;", "if (type == ARC_TBZ) return bzip2_decode;"),
    ("empty bz2", "&& s->type != ARC_XZ && s->type != ARC_BZ)", "&& s->type != ARC_XZ)"),
    ("zip method 12", "} else if (it->method == 12) {", "} else if (0) {"),
]
MUT_SRC = ["src/fs/arc_detect.c", "src/fs/arc_source.c", "src/fs/arc_extract.c"]


def test_mutants(work: Path) -> None:
    srcs = {f: (ROOT / f).read_text() for f in MUT_SRC}
    for name, old, _ in MUTANTS:
        assert any(old in t for t in srcs.values()), f"mutation anchor vanished: {name}"
    d = work / "mm"
    make_fixtures(d)
    (d / "z12.zip").write_bytes(zip_with_methods([("a.txt", TEXT, 12)]))
    script = f"N {d}\n" + "".join(f"E {n}\nX a.txt\nX b.txt\nX notes.txt\nX multi\nX blocks\nX empty\nX notes.txt.bz2.out\nU\n" for n in sorted(os.listdir(d)))
    base = V.run(V.build(work / "mbase"), script, timeout=60)
    survivors: list[str] = []
    saved_new, saved_vfs = V.NEW, V.VFS
    V.NEW = [x for x in V.NEW if x not in MUT_SRC]
    V.VFS = [x for x in V.VFS if x not in MUT_SRC]

    def job(i: int, name: str, old: str, new: str):
        def go() -> None:
            md = work / f"m{i}"
            md.mkdir()
            patched, extra = False, []
            for f in MUT_SRC:
                text = srcs[f]
                if not patched and old in text:
                    text, patched = text.replace(old, new, 1), True
                p = md / Path(f).name
                p.write_text(text)
                extra.append(str(p))
            try:
                same = V.run(V.build(md, vfs=[*V.VFS, *extra]), script, timeout=60) == base
            except (subprocess.CalledProcessError, subprocess.TimeoutExpired, AssertionError):
                same = False
            if same:
                survivors.append(name)
        return go

    try:
        H.parallel([job(i, *m) for i, m in enumerate(MUTANTS)])
    finally:
        V.NEW, V.VFS = saved_new, saved_vfs
    assert not survivors, f"mutants not noticed: {sorted(survivors)}"
    print(f"[PASS] BA5. {len(MUTANTS)}/{len(MUTANTS)} mutants of the bzip2 archive glue are noticed")


def test_open_caps(work: Path) -> None:
    """The open is synchronous, so the slower codecs get a smaller share of ARC_OPEN_TGZ_MAX: xz half, bzip2 a quarter."""
    d = work / "caps"
    d.mkdir()
    from xz_support import xz
    tar = tar_bytes([Expect("blob.bin", data=random.Random(4).randbytes(700000))])             # 700 KB unpacked
    for name, data in (("c.tar.gz", gzip.compress(tar, 1, mtime=0)), ("c.tar.xz", xz(tar, "crc32", preset=1)), ("c.tar.bz2", bz(tar, 1)), ("s.bz2", bz(tar, 1)),
                       ("small.tar.bz2", bz(tar_bytes([Expect("a", data=b"x" * 100000)]), 1))):
        (d / name).write_bytes(data)
    exe = V.run(V.build(work / "caps_b", cflags=["-DARC_OPEN_TGZ_MAX=1048576ull"]), f"N {d}\nE c.tar.gz\nU\nE c.tar.xz\nU\nE c.tar.bz2\nU\nE s.bz2\nU\nE small.tar.bz2\n")
    got = [int(exe[i].split()[1][2:]) for i in (1, 3, 5, 7, 9)]
    assert got == [1, 5, 5, 5, 1], got                    # of 1 MiB: gzip all, xz 512 KiB, bzip2 256 KiB; the tar is 700 KB
    print("[PASS] BA6. the synchronous open caps what a codec may unpack: bzip2 a quarter of ARC_OPEN_TGZ_MAX, xz half, the rest all of it")


def main() -> None:
    with H.workdir() as t:
        w = Path(t).resolve()
        exe = V.build(w / "x86")
        test_plain_and_tar(exe, w)
        test_zip12(exe, w)
        test_unpack(w)
        test_aarch64(exe, w)
        test_open_caps(w)
        test_mutants(w)
    print("ALL C ARC BZIP2 TESTS PASSED.")


if __name__ == "__main__":
    sys.exit(main())
