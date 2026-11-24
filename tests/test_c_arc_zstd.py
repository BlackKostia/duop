#!/usr/bin/env python3
"""Zstandard in the archive layer (src/fs/arc_detect.c, arc_source.c, arc_extract.c on src/fs/zstd_*.c; docs/ARCHIVE_ROADMAP_2026-11-24.md, phase 4).

ZA1 a plain .zst is a one-member archive, a .tar.zst / .tzst (or a tar.zst under any name, even when its first block is stored) a tar:
    names, exact bytes, several frames, a skippable frame first, an empty payload; a bad checksum / cut / opaque file is not entered (r=4)
ZA2 zip members compressed with method 93 list, extract to their exact bytes and fail on a wrong CRC
ZA3 unpacking and testing: .zst, .tar.zst, a renamed one, a zip with a zstd member; damaged input leaves nothing
ZA4 aarch64 (qemu-user) gives the x86_64 results;  ZA5 mutation check
"""

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

ZSTD = "zstd"


def zst(data: bytes, *args: str) -> bytes:
    return subprocess.run([ZSTD, "-q", "-c", *args], input=data, capture_output=True, check=True).stdout


def zip_with_method(members: list[tuple[str, bytes, int]]) -> bytes:
    """A zip whose members are stored (0) or Zstandard (93) compressed - Python's zipfile cannot write method 93."""
    out, central = bytearray(), bytearray()
    for name, data, method in members:
        comp = zst(data) if method == 93 else data
        off, nm = len(out), name.encode()
        crc = zlib.crc32(data)
        out += struct.pack("<IHHHHHIIIHH", 0x04034B50, 63 if method == 93 else 20, 0, method, 0, 0x21, crc, len(comp), len(data), len(nm), 0) + nm + comp
        central += struct.pack("<IHHHHHHIIIHHHHHII", 0x02014B50, 63, 63 if method == 93 else 20, 0, method, 0, 0x21, crc, len(comp), len(data), len(nm), 0, 0, 0, 0, 0, off) + nm
    cd = len(out)
    return bytes(out + central + struct.pack("<IHHHHIIH", 0x06054B50, 0, 0, len(members), len(members), len(central), cd, 0))


def make_fixtures(d: Path) -> None:
    d.mkdir()
    rnd = random.Random(3)
    tar = tar_bytes(ENTRIES)
    big_tar = tar_bytes([Expect("blob.bin", data=rnd.randbytes(400000)), Expect("b.txt", data=b"BBB")])      # the first zstd block is stored
    (d / "notes.txt.zst").write_bytes(zst(TEXT))
    (d / "blob.dat").write_bytes(zst(rnd.randbytes(30000)))
    (d / "multi.zst").write_bytes(zst(b"first part\n") + zst(b"second part\n"))
    (d / "skip.zst").write_bytes(struct.pack("<II", 0x184D2A50, 4) + b"meta" + zst(b"after the skippable frame\n"))
    (d / "skipfirst.dat").write_bytes(struct.pack("<II", 0x184D2A5F, 3) + b"abc" + zst(b"after skippable too\n"))     # no .zst in the name: only the magic says
    (d / "empty.zst").write_bytes(zst(b""))
    (d / "a.tar.zst").write_bytes(zst(tar))
    (d / "b.tzst").write_bytes(zst(tar))
    (d / "tar_as.dat").write_bytes(zst(tar))
    (d / "tar_in.zst").write_bytes(zst(tar))                                                                # a tar under a plain .zst name: still a tar
    (d / "big.dat").write_bytes(zst(big_tar, "-1"))
    (d / "zeros.tzst").write_bytes(zst(bytes(1024)))                                                        # a tar of no members: the name settles it
    bad = bytearray(zst(TEXT))
    bad[-1] ^= 1
    (d / "badsum.zst").write_bytes(bytes(bad))
    (d / "trunc.zst").write_bytes(zst(rnd.randbytes(20000))[:6000])
    (d / "opaque.zst").write_bytes(b"opaque " * 200)
    (d / "empty0.zst").write_bytes(b"")


def test_plain_and_tar(exe: Path, work: Path) -> None:
    d = work / "z"
    make_fixtures(d)
    singles = {"notes.txt.zst": ("notes.txt", TEXT), "blob.dat": ("blob.dat.out", None), "multi.zst": ("multi", b"first part\nsecond part\n"),
               "skip.zst": ("skip", b"after the skippable frame\n"), "empty.zst": ("empty", b""),
               "skipfirst.dat": ("skipfirst.dat.out", b"after skippable too\n")}
    tars = ("a.tar.zst", "b.tzst", "tar_as.dat", "tar_in.zst")
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
        elif n == "zeros.tzst":
            assert e.startswith("E r=1 ") and kids(e) == {}, (n, e[:140])
            i += 2
        elif n == "big.dat":
            assert e.startswith("E r=1 ") and kids(e) == {b"blob.bin": ("f", 400000), b"b.txt": ("f", 3)}, (n, e[:140])
            assert out[i + 1] == f"X r=0 len=3 crc={zlib.crc32(b'BBB')}", (n, out[i + 1])
            i += 3
        else:
            assert e.startswith("E r=4 ") and " vm=0 " in e, (n, e[:100])
            i += 2
    print(f"[PASS] ZA1. .zst, .tar.zst, .tzst and zstd under any name: {len(singles)} one-member archives, {len(tars) + 1} tars (also with a stored first block); damaged files are r=4")


def test_zip93(exe: Path, work: Path) -> None:
    d = work / "zip"
    d.mkdir()
    data = {"a.txt": TEXT, "dir/b.bin": random.Random(8).randbytes(70000), "empty": b""}
    (d / "z93.zip").write_bytes(zip_with_method([(n, b, 93 if b else 0) for n, b in data.items()]))
    z = bytearray(zip_with_method([("c.txt", TEXT, 93)]))
    z[z.index(b"c.txt") + 5 + 12] ^= 0xFF                                           # a byte inside the zstd data
    (d / "bad93.zip").write_bytes(bytes(z))
    out = V.run(exe, f"N {d}\nE z93.zip\nX a.txt\nE dir\nX b.bin\nU\nU\nE bad93.zip\nX c.txt\n")
    assert out[1].startswith("E r=1 "), out[1][:100]
    assert out[2] == f"X r=0 len={len(TEXT)} crc={zlib.crc32(TEXT)}", out[2]
    assert out[4] == f"X r=0 len=70000 crc={zlib.crc32(data['dir/b.bin'])}", out[4]
    assert out[7].startswith("E r=1 ") and re.match(r"X r=-(22|74) ", out[8] + " "), out[7:9]
    print("[PASS] ZA2. zip members with method 93 list and extract to their exact bytes; a damaged one fails (-EINVAL / -EBADMSG)")


def test_unpack(work: Path) -> None:
    d = work / "unp"
    d.mkdir()
    tar = tar_bytes(ENTRIES)
    (d / "notes.txt.zst").write_bytes(zst(TEXT))
    (d / "a.tar.zst").write_bytes(zst(tar))
    (d / "renamed.dat").write_bytes(zst(tar))
    (d / "z93.zip").write_bytes(zip_with_method([("a.txt", TEXT, 93), ("b.txt", b"BBB", 0)]))
    bad = bytearray(zst(TEXT))
    bad[-1] ^= 1
    (d / "bad.zst").write_bytes(bytes(bad))
    exe = US.build(work / "ux86")
    script = ""
    for n in ("notes.txt.zst", "a.tar.zst", "renamed.dat", "z93.zip", "bad.zst"):
        (work / "o" / n).mkdir(parents=True)
        script += US.U(d / n, work / "o" / n, smart=1)
    script += f"T {d / 'notes.txt.zst'}\nT {d / 'a.tar.zst'}\nT {d / 'bad.zst'}\nT {d / 'z93.zip'}\n"
    out = US.run(exe, script)
    o = work / "o"
    r = US.kv(out[0])
    assert r["rc"] == 0 and r["files"] == 1 and r["dirs"] == 0 and (o / "notes.txt.zst" / "notes.txt").read_bytes() == TEXT, out[0]
    for k, n in ((1, "a.tar.zst"), (2, "renamed.dat")):
        assert US.kv(out[k])["rc"] == 0, out[k]
        assert [p.read_bytes() for p in sorted((o / n).rglob("*.txt"))] == [b"AAA", b"BBB"], (n, sorted(str(p) for p in (o / n).rglob("*")))
    assert US.kv(out[3])["rc"] == 0 and next((o / "z93.zip").rglob("a.txt")).read_bytes() == TEXT and next((o / "z93.zip").rglob("b.txt")).read_bytes() == b"BBB", out[3]
    assert US.kv(out[4])["rc"] != 0 and os.listdir(o / "bad.zst") == [], f"a bad zstd must leave nothing: {out[4]}"
    t = [US.kv(x) for x in out[5:9]]
    assert t[0]["rc"] == 0 and t[0]["bytes"] == len(TEXT) and t[1]["rc"] == 0 and t[1]["files"] == 2, out[5:7]
    assert t[2]["rc"] != 0 and t[3]["rc"] == 0 and t[3]["files"] == 2, out[7:9]
    print("[PASS] ZA3. .zst, .tar.zst (also renamed) and a zip with a zstd member unpack and test; a bad .zst leaves nothing")


MUTANTS = [
    ("zstd magic", "h[0] == 0x28 && h[1] == 0xB5", "h[0] == 0x29 && h[1] == 0xB5"),
    ("skippable first", "((h[0] & 0xF0) == 0x50", "((h[0] & 0xF0) == 0x60"),
    ("first block peek", "#define ZST_PEEK (144u << 10)", "#define ZST_PEEK (64u << 10)"),
    ("tzst name", 'ends_ci(path, ".tzst")', 'ends_ci(path, ".tzs")'),
    ("zst name", 'if (ends_ci(path, ".zst")) return ARC_ZST;', ""),
    ("zero block of tar.zst", "(by_ext == tar_type && all_zero(pk.blk, 512))", "false"),
    ("single member name", 'static const char *const sfx[] = {".gz", ".zst", ".xz", ".bz2", ".tgz", ".tzst", ".txz", ".tbz2", ".tbz"};', 'static const char *const sfx[] = {".gz", ".xz", ".bz2", ".tgz", ".tzst", ".txz", ".tbz2", ".tbz"};'),
    ("zstd decoder of zst", "if (type == ARC_TZST || type == ARC_ZST) return zstd_decode;", "if (type == ARC_TZST) return zstd_decode;"),
    ("empty zst", "&& s->type != ARC_GZ && s->type != ARC_ZST && s->type != ARC_XZ && s->type != ARC_BZ)", "&& s->type != ARC_GZ && s->type != ARC_XZ && s->type != ARC_BZ)"),
    ("zip method 93", "} else if (it->method == 93) {", "} else if (0) {"),
]
MUT_SRC = ["src/fs/arc_detect.c", "src/fs/arc_source.c", "src/fs/arc_extract.c"]


def test_mutants(work: Path) -> None:
    srcs = {f: (ROOT / f).read_text() for f in MUT_SRC}
    for name, old, _ in MUTANTS:
        assert any(old in t for t in srcs.values()), f"mutation anchor vanished: {name}"
    d = work / "mm"
    make_fixtures(d)
    (d / "z93.zip").write_bytes(zip_with_method([("a.txt", TEXT, 93)]))
    names = sorted(os.listdir(d))
    script = f"N {d}\n" + "".join(f"E {n}\nX a.txt\nX b.txt\nX notes.txt\nX notes.txt.zst.out\nX multi\nX skip\nX empty\nX skipfirst.dat.out\nU\n" for n in names)
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
    print(f"[PASS] ZA5. {len(MUTANTS)}/{len(MUTANTS)} mutants of the zstd archive glue are noticed")


def test_aarch64(x86: Path, work: Path) -> None:
    if not H.available("aarch64"):
        print("[SKIP] ZA4. aarch64 toolchain / qemu-user missing")
        return
    d = work / "a64"
    d.mkdir()
    (d / "n.txt.zst").write_bytes(zst(TEXT))
    (d / "t.zst").write_bytes(zst(tar_bytes(ENTRIES)))
    (d / "z.zip").write_bytes(zip_with_method([("a.txt", TEXT, 93)]))
    script = f"N {d}\n" + "".join(f"E {n}\nL\nX {m}\nU\n" for n, m in (("n.txt.zst", "n.txt"), ("t.zst", "a.txt"), ("z.zip", "a.txt")))
    a, b = V.run(V.build(work / "a64b", "aarch64"), script, "aarch64"), V.run(x86, script)
    assert a == b, "aarch64 and x86_64 disagree"
    print("[PASS] ZA4. zstd archives give the same output under qemu-aarch64")


def main() -> None:
    import shutil
    if not shutil.which(ZSTD):
        print("[SKIP] the zstd command is missing")
        return
    with H.workdir() as t:
        w = Path(t).resolve()
        exe = V.build(w / "x86")
        test_plain_and_tar(exe, w)
        test_zip93(exe, w)
        test_unpack(w)
        test_aarch64(exe, w)
        test_mutants(w)
    print("ALL C ARC ZSTD TESTS PASSED.")


if __name__ == "__main__":
    sys.exit(main())
