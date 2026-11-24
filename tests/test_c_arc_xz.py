#!/usr/bin/env python3
"""xz in the archive layer (src/fs/arc_detect.c, arc_source.c, arc_extract.c on src/fs/xz_dec.c; docs/ARCHIVE_ROADMAP_2026-11-24.md, phase 4).

XA1 a plain .xz is a one-member archive, a .tar.xz / .txz (or a tar.xz under any name, even when its first LZMA2 chunk is stored) a tar:
    names, exact bytes, several streams and blocks, an empty payload; a bad check / cut / opaque file is not entered (r=4)
XA2 zip members compressed with method 95 (xz) list, extract to their exact bytes and fail on a wrong CRC
XA3 unpacking and testing: .xz, .tar.xz, a renamed one, a zip with an xz member; damaged input leaves nothing
XA4 aarch64 (qemu-user) gives the x86_64 results;  XA5 mutation check
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
from xz_support import xz, xz_cli


def zip_with_methods(members: list[tuple[str, bytes, int]]) -> bytes:
    """A zip whose members are stored (0) or xz (95) compressed - Python's zipfile cannot write method 95."""
    out, central = bytearray(), bytearray()
    for name, data, method in members:
        comp = xz(data, "crc32", preset=1) if method == 95 else data
        off, nm, crc = len(out), name.encode(), zlib.crc32(data)
        out += struct.pack("<IHHHHHIIIHH", 0x04034B50, 63, 0, method, 0, 0x21, crc, len(comp), len(data), len(nm), 0) + nm + comp
        central += struct.pack("<IHHHHHHIIIHHHHHII", 0x02014B50, 63, 63, 0, method, 0, 0x21, crc, len(comp), len(data), len(nm), 0, 0, 0, 0, 0, off) + nm
    cd = len(out)
    return bytes(out + central + struct.pack("<IHHHHIIH", 0x06054B50, 0, 0, len(members), len(members), len(central), cd, 0))


def make_fixtures(d: Path) -> None:
    d.mkdir()
    rnd = random.Random(3)
    tar = tar_bytes(ENTRIES)
    big_tar = tar_bytes([Expect("blob.bin", data=rnd.randbytes(400000)), Expect("b.txt", data=b"BBB")])      # the first LZMA2 chunks are stored
    (d / "notes.txt.xz").write_bytes(xz(TEXT))
    (d / "blob.dat").write_bytes(xz(rnd.randbytes(30000), "crc32"))
    (d / "multi.xz").write_bytes(xz(b"first part\n", "none") + xz(b"second part\n", "sha256"))
    (d / "blocks.xz").write_bytes(xz_cli(TEXT * 40, "-1", "--block-size=4KiB"))
    (d / "empty.xz").write_bytes(xz(b""))
    (d / "a.tar.xz").write_bytes(xz(tar))
    (d / "b.txz").write_bytes(xz(tar, "crc32"))
    (d / "tar_as.dat").write_bytes(xz(tar, "none"))
    (d / "tar_in.xz").write_bytes(xz(tar))                                                                   # a tar under a plain .xz name: still a tar
    (d / "big.dat").write_bytes(xz(big_tar, "crc64", preset=1))
    (d / "zeros.txz").write_bytes(xz(bytes(1024)))                                                           # a tar of no members: the name settles it
    bad = bytearray(xz(TEXT, "crc32"))
    bad[-12 - 8] ^= 1
    (d / "badsum.xz").write_bytes(bytes(bad))
    (d / "trunc.xz").write_bytes(xz(rnd.randbytes(20000))[:6000])
    (d / "opaque.xz").write_bytes(b"opaque " * 200)
    (d / "empty0.xz").write_bytes(b"")


def test_plain_and_tar(exe: Path, work: Path) -> None:
    d = work / "z"
    make_fixtures(d)
    singles = {"notes.txt.xz": ("notes.txt", TEXT), "blob.dat": ("blob.dat.out", None), "multi.xz": ("multi", b"first part\nsecond part\n"),
               "blocks.xz": ("blocks", TEXT * 40), "empty.xz": ("empty", b"")}
    tars = ("a.tar.xz", "b.txz", "tar_as.dat", "tar_in.xz")
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
        elif n == "zeros.txz":
            assert e.startswith("E r=1 ") and kids(e) == {}, (n, e[:140])
            i += 2
        elif n == "big.dat":
            assert e.startswith("E r=1 ") and kids(e) == {b"blob.bin": ("f", 400000), b"b.txt": ("f", 3)}, (n, e[:140])
            assert out[i + 1] == f"X r=0 len=3 crc={zlib.crc32(b'BBB')}", (n, out[i + 1])
            i += 3
        else:
            assert e.startswith("E r=4 ") and " vm=0 " in e, (n, e[:100])
            i += 2
    print(f"[PASS] XA1. .xz, .tar.xz, .txz and xz under any name: {len(singles)} one-member archives, {len(tars) + 1} tars; damaged files are r=4")


def test_zip95(exe: Path, work: Path) -> None:
    d = work / "zip"
    d.mkdir()
    data = {"a.txt": TEXT, "dir/b.bin": random.Random(8).randbytes(70000), "empty": b""}
    (d / "z95.zip").write_bytes(zip_with_methods([(n, b, 95 if b else 0) for n, b in data.items()]))
    z = bytearray(zip_with_methods([("c.txt", TEXT, 95)]))
    z[z.index(b"c.txt") + 5 + 40] ^= 0xFF                                           # a byte inside the xz data
    (d / "bad95.zip").write_bytes(bytes(z))
    out = V.run(exe, f"N {d}\nE z95.zip\nX a.txt\nE dir\nX b.bin\nU\nU\nE bad95.zip\nX c.txt\n")
    assert out[1].startswith("E r=1 "), out[1][:100]
    assert out[2] == f"X r=0 len={len(TEXT)} crc={zlib.crc32(TEXT)}", out[2]
    assert out[4] == f"X r=0 len=70000 crc={zlib.crc32(data['dir/b.bin'])}", out[4]
    assert out[7].startswith("E r=1 ") and re.match(r"X r=-(22|74) ", out[8] + " "), out[7:9]
    print("[PASS] XA2. zip members with method 95 list and extract to their exact bytes; a damaged one fails (-EINVAL / -EBADMSG)")


def test_unpack(work: Path) -> None:
    d = work / "unp"
    d.mkdir()
    tar = tar_bytes(ENTRIES)
    (d / "notes.txt.xz").write_bytes(xz(TEXT))
    (d / "a.tar.xz").write_bytes(xz(tar))
    (d / "renamed.dat").write_bytes(xz(tar))
    (d / "z95.zip").write_bytes(zip_with_methods([("a.txt", TEXT, 95), ("b.txt", b"BBB", 0)]))
    bad = bytearray(xz(TEXT, "crc32"))
    bad[-12 - 8] ^= 1
    (d / "bad.xz").write_bytes(bytes(bad))
    exe = US.build(work / "ux86")
    script = ""
    for n in ("notes.txt.xz", "a.tar.xz", "renamed.dat", "z95.zip", "bad.xz"):
        (work / "o" / n).mkdir(parents=True)
        script += US.U(d / n, work / "o" / n, smart=1)
    script += f"T {d / 'notes.txt.xz'}\nT {d / 'a.tar.xz'}\nT {d / 'bad.xz'}\nT {d / 'z95.zip'}\n"
    out = US.run(exe, script)
    o = work / "o"
    r = US.kv(out[0])
    assert r["rc"] == 0 and r["files"] == 1 and r["dirs"] == 0 and (o / "notes.txt.xz" / "notes.txt").read_bytes() == TEXT, out[0]
    for k, n in ((1, "a.tar.xz"), (2, "renamed.dat")):
        assert US.kv(out[k])["rc"] == 0, out[k]
        assert [p.read_bytes() for p in sorted((o / n).rglob("*.txt"))] == [b"AAA", b"BBB"], (n, sorted(str(p) for p in (o / n).rglob("*")))
    assert US.kv(out[3])["rc"] == 0 and next((o / "z95.zip").rglob("a.txt")).read_bytes() == TEXT and next((o / "z95.zip").rglob("b.txt")).read_bytes() == b"BBB", out[3]
    assert US.kv(out[4])["rc"] != 0 and os.listdir(o / "bad.xz") == [], f"a bad xz must leave nothing: {out[4]}"
    t = [US.kv(x) for x in out[5:9]]
    assert t[0]["rc"] == 0 and t[0]["bytes"] == len(TEXT) and t[1]["rc"] == 0 and t[1]["files"] == 2, out[5:7]
    assert t[2]["rc"] != 0 and t[3]["rc"] == 0 and t[3]["files"] == 2, out[7:9]
    print("[PASS] XA3. .xz, .tar.xz (also renamed) and a zip with an xz member unpack and test; a bad .xz leaves nothing")


def test_aarch64(x86: Path, work: Path) -> None:
    if not H.available("aarch64"):
        print("[SKIP] XA4. aarch64 toolchain / qemu-user missing")
        return
    d = work / "a64"
    d.mkdir()
    (d / "n.txt.xz").write_bytes(xz(TEXT))
    (d / "t.xz").write_bytes(xz(tar_bytes(ENTRIES)))
    (d / "z.zip").write_bytes(zip_with_methods([("a.txt", TEXT, 95)]))
    script = f"N {d}\n" + "".join(f"E {n}\nL\nX {m}\nU\n" for n, m in (("n.txt.xz", "n.txt"), ("t.xz", "a.txt"), ("z.zip", "a.txt")))
    a, b = V.run(V.build(work / "a64b", "aarch64"), script, "aarch64"), V.run(x86, script)
    assert a == b, "aarch64 and x86_64 disagree"
    print("[PASS] XA4. xz archives give the same output under qemu-aarch64")


MUTANTS = [
    ("xz magic", "static const uint8_t xz_magic[] = {0xFD, '7', 'z', 'X', 'Z', 0};", "static const uint8_t xz_magic[] = {0xFD, '7', 'z', 'X', 'Y', 0};"),
    ("first chunk peek", "#define XZ_PEEK (96u << 10)", "#define XZ_PEEK (8u << 10)"),
    ("txz name", 'ends_ci(path, ".txz")', 'ends_ci(path, ".tx")'),
    ("xz name", 'return ends_ci(path, ".xz") ? ARC_XZ : 0;', "return 0;"),
    ("single member name", 'static const char *const sfx[] = {".gz", ".zst", ".xz", ".tgz", ".tzst", ".txz"};', 'static const char *const sfx[] = {".gz", ".zst", ".tgz", ".tzst", ".txz"};'),
    ("xz decoder", "if (type == ARC_TXZ || type == ARC_XZ) return xz_decode;", "if (type == ARC_TXZ) return xz_decode;"),
    ("empty xz", "&& s->type != ARC_ZST && s->type != ARC_XZ)", "&& s->type != ARC_ZST)"),
    ("zip method 95", "} else if (it->method == 95) {", "} else if (0) {"),
]
MUT_SRC = ["src/fs/arc_detect.c", "src/fs/arc_source.c", "src/fs/arc_extract.c"]


def test_mutants(work: Path) -> None:
    srcs = {f: (ROOT / f).read_text() for f in MUT_SRC}
    for name, old, _ in MUTANTS:
        assert any(old in t for t in srcs.values()), f"mutation anchor vanished: {name}"
    d = work / "mm"
    make_fixtures(d)
    (d / "z95.zip").write_bytes(zip_with_methods([("a.txt", TEXT, 95)]))
    script = f"N {d}\n" + "".join(f"E {n}\nX a.txt\nX b.txt\nX notes.txt\nX multi\nX blocks\nX empty\nX notes.txt.xz.out\nU\n" for n in sorted(os.listdir(d)))
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
    print(f"[PASS] XA5. {len(MUTANTS)}/{len(MUTANTS)} mutants of the xz archive glue are noticed")


def main() -> None:
    import shutil
    if not shutil.which("xz"):
        print("[SKIP] the xz command is missing")
        return
    with H.workdir() as t:
        w = Path(t).resolve()
        exe = V.build(w / "x86")
        test_plain_and_tar(exe, w)
        test_zip95(exe, w)
        test_unpack(w)
        test_aarch64(exe, w)
        test_mutants(w)
    print("ALL C ARC XZ TESTS PASSED.")


if __name__ == "__main__":
    sys.exit(main())
