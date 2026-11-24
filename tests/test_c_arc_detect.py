#!/usr/bin/env python3
"""src/fs/arc_detect.c + arc_source.c (single-file gzip): the kind of an archive is decided by its contents, and a plain .gz is a
one-member archive (docs/ARCHIVE_ROADMAP_2026-11-24.md, phase 3).

DT1 renamed archives: zip / tar / tar.gz under any name, a tar named .zip, a zip named .tar, a tar.gz named .gz are entered as what they are
DT2 plain .gz: one member named after the file (.gz stripped; "<name>.out" under another name), exact bytes, multi-member gzip, an empty
    payload; the modification time of the gzip header is kept; a bad CRC / truncated stream is not entered (r=4)
DT3 xz / bzip2 / 7z / rar by magic are "not supported yet" (r=6) under any name; text that only looks similar is a plain file (r=0)
DT4 a FIFO named like an archive never blocks the open; empty and tiny files are not archives
DT5 unpacking and testing a plain .gz (smart unpack: no subfolder), a renamed zip, a tar.gz named .gz;  DT6 aarch64 equals x86_64
DT7 mutation check
"""

import gzip
import hashlib
import io
import os
import random
import shutil
import subprocess
import sys
import zipfile
import zlib
from pathlib import Path

import arcunp_support as US
import c_harness as H
import test_c_arc_vfs as V
from archive_fixtures import Expect, tar_bytes, zip_bytes

ROOT = H.ROOT
TEXT = b"".join(b"line %05d of the notes\n" % i for i in range(300))
BLOB = bytes((i * 7 + (i >> 8)) & 0xFF for i in range(150000))
ENTRIES = [Expect("a.txt", data=b"AAA"), Expect("sub/b.txt", data=b"BBB")]
MT = 1_600_000_000


def crc(b: bytes) -> int:
    return zlib.crc32(b)


def kids(line: str) -> dict[bytes, tuple[str, int]]:
    return V.listing(line)[1]


def enter_all(exe: Path, d: Path, names: list[str], extra: str = "") -> dict[str, list[str]]:
    """{name: [E line, X lines..., U line]} for each archive: enter, extract what `extra` names (per archive), leave."""
    script = f"N {d}\n"
    for n in names:
        script += f"E {n}\n{extra}U\n"
    out = V.run(exe, script)[1:]
    res, i = {}, 0
    per = 2 + extra.count("\n")
    for n in names:
        res[n] = out[i:i + per]
        i += per
    return res


def test_renamed(exe: Path, work: Path) -> None:
    d = work / "ren"
    d.mkdir()
    tar = tar_bytes(ENTRIES)
    zp = zip_bytes(ENTRIES)
    buf = io.BytesIO()
    zipfile.ZipFile(buf, "w").close()
    files = {"empty_zip.dat": buf.getvalue(), "zip_as.dat": zp, "tar_as.bin": tar, "tgz_as.dat": gzip.compress(tar, 6, mtime=0), "tgz_as.gz": gzip.compress(tar, 6, mtime=0),
             "tar_is.zip": tar, "zip_is.tar": zp, "zip_noext": zp}
    for n, b in files.items():
        (d / n).write_bytes(b)
    res = enter_all(exe, d, sorted(files), "X a.txt\n")
    for n, (e, x, _u) in res.items():
        assert e.startswith("E r=1 "), (n, e[:80])
        if n == "empty_zip.dat":
            assert kids(e) == {}, e[:120]                                                 # an empty zip: entered, nothing in it
            continue
        assert kids(e) == {b"a.txt": ("f", 3), b"sub": ("d", 0)}, (n, kids(e))
        assert x == f"X r=0 len=3 crc={crc(b'AAA')}", (n, x)
    print(f"[PASS] DT1. {len(files)} archives under the wrong or no extension are entered as what their contents say")


def test_plain_gz(exe: Path, work: Path) -> None:
    d = work / "gz"
    d.mkdir()
    (d / "notes.txt.gz").write_bytes(gzip.compress(TEXT, 6, mtime=MT))
    (d / "blob.dat").write_bytes(gzip.compress(BLOB, 1, mtime=0))
    (d / "multi.gz").write_bytes(gzip.compress(b"first part\n", mtime=0) + gzip.compress(b"second part\n", mtime=0))
    (d / "empty.gz").write_bytes(gzip.compress(b"", mtime=0))
    (d / "UPPER.TXT.GZ").write_bytes(gzip.compress(b"shout\n", mtime=0))
    (d / "a.b.c.gz").write_bytes(gzip.compress(b"dots\n", mtime=0))
    (d / "tar_in.gz").write_bytes(gzip.compress(tar_bytes(ENTRIES), 6, mtime=0))
    bad = bytearray(gzip.compress(TEXT, 6, mtime=0))
    bad[-6] ^= 0x55
    (d / "badcrc.gz").write_bytes(bytes(bad))
    (d / "trunc.gz").write_bytes(gzip.compress(random.Random(5).randbytes(20000), 1, mtime=0)[:5000])
    (d / "empty0.gz").write_bytes(b"")
    (d / "tiny.tgz").write_bytes(gzip.compress(b"short", mtime=0))                       # too short for a tar, whatever the name says: one member
    (d / "z100.tgz").write_bytes(gzip.compress(bytes(100), mtime=0))
    (d / "zeros.tgz").write_bytes(gzip.compress(bytes(1024), 6, mtime=0))                # a tar of no members: entered, empty (not a member of zeros)
    names = ["notes.txt.gz", "blob.dat", "multi.gz", "empty.gz", "UPPER.TXT.GZ", "a.b.c.gz", "tar_in.gz", "badcrc.gz", "trunc.gz", "empty0.gz", "zeros.tgz", "tiny.tgz", "z100.tgz"]
    members = {"notes.txt.gz": ("notes.txt", TEXT), "blob.dat": ("blob.dat.out", BLOB), "multi.gz": ("multi", b"first part\nsecond part\n"),
               "empty.gz": ("empty", b""), "tiny.tgz": ("tiny", b"short"), "z100.tgz": ("z100", bytes(100)), "UPPER.TXT.GZ": ("UPPER.TXT", b"shout\n"), "a.b.c.gz": ("a.b.c", b"dots\n")}
    script = f"N {d}\n" + "".join(f"E {n}\n" + (f"X {members[n][0]}\n" if n in members else "") + "U\n" for n in names)
    out = V.run(exe, script)[1:]
    i = 0
    for n in names:
        e = out[i]
        if n in members:
            m, data = members[n]
            assert e.startswith("E r=1 ") and kids(e) == {m.encode(): ("f", len(data))}, (n, e[:140])
            assert out[i + 1] == f"X r=0 len={len(data)} crc={crc(data)}", (n, out[i + 1])
            i += 3
        elif n == "tar_in.gz":
            assert e.startswith("E r=1 ") and kids(e) == {b"a.txt": ("f", 3), b"sub": ("d", 0)}, (n, e[:140])
            i += 2
        elif n == "zeros.tgz":
            assert e.startswith("E r=1 ") and kids(e) == {}, (n, e[:140])
            i += 2
        elif n == "empty0.gz":
            assert e.startswith("E r=0 ") and " vm=0 " in e, (n, e[:100])             # no bytes, no gzip: an ordinary (empty) file
            i += 2
        else:
            assert e.startswith("E r=4 ") and " vm=0 " in e, (n, e[:100])             # damaged: recognised, unreadable
            i += 2
    print("[PASS] DT2. a plain .gz is a one-member archive: name, exact bytes, multi-member, empty payload; bad CRC / cut stream are r=4, an empty file is a plain file")


def test_unsupported(exe: Path, work: Path) -> None:
    d = work / "uns"
    d.mkdir()
    src = d / "payload.txt"
    src.write_bytes(TEXT)
    made = {}
    for tool, args, ext in (("xz", ["xz", "-c"], "x1.dat"), ("bzip2", ["bzip2", "-c"], "x2.dat")):
        if shutil.which(tool):
            made[ext] = subprocess.run([*args, str(src)], capture_output=True, check=True).stdout
    if shutil.which("7z"):
        subprocess.run(["7z", "a", "-bd", "-bso0", str(d / "x4.7z"), str(src)], check=True, capture_output=True)
        made["x4.dat"] = (d / "x4.7z").read_bytes()
        (d / "x4.7z").unlink()
    made["x5.dat"] = b"Rar!\x1a\x07\x01\x00" + bytes(200)
    made["x6.txt"] = b"\xfd7zXZ\x00" + bytes(100)                                       # magic wins over the extension
    src.unlink()
    for n, b in made.items():
        (d / n).write_bytes(b)
    plain = {"fake_bz.dat": b"BZh9 is just how this text begins\n", "fake_pk.dat": b"PK is not a zip header\n", "fake_gz.dat": b"\x1f is not gzip\n",
             "notes.txt": TEXT, "random.bin": bytes(range(256)) * 20, "one.dat": b"x", "nul.dat": b""}
    for n, b in plain.items():
        (d / n).write_bytes(b)
    out = V.run(exe, f"N {d}\n" + "".join(f"E {n}\nU\n" for n in [*made, *plain]))[1:]
    for i, n in enumerate([*made, *plain]):
        want = 6 if n in made else 0
        assert out[2 * i].startswith(f"E r={want} "), (n, out[2 * i][:60])
    print(f"[PASS] DT3. {len(made)} xz / bzip2 / zstd / 7z / rar files are r=6 under any name; {len(plain)} look-alikes are plain files (r=0)")


def test_not_blocking(exe: Path, work: Path) -> None:
    d = work / "fifo"
    d.mkdir()
    os.mkfifo(d / "pipe.tar")
    os.mkfifo(d / "pipe.dat")
    (d / "keep.zip").write_bytes(zip_bytes(ENTRIES))
    res = H.run("x86_64", exe, [], f"N {d}\nE pipe.tar\nE pipe.dat\nE keep.zip\n".encode(), timeout=20)
    assert res.returncode == 0, res.stderr[-200:]
    lines = res.stdout.decode(errors="backslashreplace").splitlines()
    assert lines[1].split()[1] in ("r=0", "r=4") and lines[2].split()[1] == "r=0" and lines[3].startswith("E r=1 "), lines[1:4]
    print("[PASS] DT4. a FIFO named like an archive does not block the open")


def sha(p: Path) -> str:
    return hashlib.sha256(p.read_bytes()).hexdigest()


def test_unpack(work: Path) -> None:
    d = work / "unp"
    d.mkdir()
    (d / "notes.txt.gz").write_bytes(gzip.compress(TEXT, 6, mtime=MT))
    (d / "blob.dat").write_bytes(gzip.compress(BLOB, 1, mtime=0))
    (d / "zip_as.dat").write_bytes(zip_bytes(ENTRIES))
    (d / "tar_in.gz").write_bytes(gzip.compress(tar_bytes(ENTRIES), 6, mtime=0))
    bad = bytearray(gzip.compress(TEXT, 6, mtime=0))
    bad[-6] ^= 1
    (d / "bad.gz").write_bytes(bytes(bad))
    exe = US.build(work / "ux86")
    script = ""
    for n in ("notes.txt.gz", "blob.dat", "zip_as.dat", "tar_in.gz", "bad.gz"):
        (work / "o" / n).mkdir(parents=True)
        script += US.U(d / n, work / "o" / n, smart=1)
    script += f"T {d / 'notes.txt.gz'}\nT {d / 'blob.dat'}\nT {d / 'bad.gz'}\nT {d / 'zip_as.dat'}\n"
    out = US.run(exe, script)
    o = work / "o"
    r = US.kv(out[0])
    assert r["rc"] == 0 and r["files"] == 1 and r["dirs"] == 0 and r["bytes"] == len(TEXT), out[0]
    assert (o / "notes.txt.gz" / "notes.txt").read_bytes() == TEXT and sorted(os.listdir(o / "notes.txt.gz")) == ["notes.txt"], os.listdir(o / "notes.txt.gz")
    assert int((o / "notes.txt.gz" / "notes.txt").stat().st_mtime) == MT, "the gzip header time is kept"
    assert US.kv(out[1])["rc"] == 0 and (o / "blob.dat" / "blob.dat.out").read_bytes() == BLOB, out[1]
    for k, n in ((2, "zip_as.dat"), (3, "tar_in.gz")):                                  # several top-level entries: smart unpack makes a subfolder
        assert US.kv(out[k])["rc"] == 0, out[k]
        assert [p.read_bytes() for p in sorted((o / n).rglob("*.txt"))] == [b"AAA", b"BBB"], (n, sorted(str(p) for p in (o / n).rglob("*")))
    assert US.kv(out[4])["rc"] != 0 and os.listdir(o / "bad.gz") == [], f"a bad gzip must leave nothing: {out[4]}"
    t = [US.kv(l) for l in out[5:9]]
    assert t[0]["rc"] == 0 and t[0]["files"] == 1 and t[0]["bytes"] == len(TEXT), out[5]
    assert t[1]["rc"] == 0 and t[1]["bytes"] == len(BLOB), out[6]
    assert t[2]["rc"] != 0, out[7]
    assert t[3]["rc"] == 0 and t[3]["files"] == 2, out[8]
    print("[PASS] DT5. a plain .gz unpacks (smart: no subfolder, header time kept) and tests; renamed zip and tar.gz-named-.gz unpack; a bad gzip leaves nothing")


def test_aarch64(x86: Path, work: Path) -> None:
    if not H.available("aarch64"):
        print("[SKIP] DT6. aarch64 toolchain / qemu-user missing")
        return
    d = work / "a64"
    d.mkdir()
    (d / "n.txt.gz").write_bytes(gzip.compress(TEXT, 6, mtime=MT))
    (d / "z.dat").write_bytes(zip_bytes(ENTRIES))
    (d / "t.gz").write_bytes(gzip.compress(tar_bytes(ENTRIES), 6, mtime=0))
    script = f"N {d}\n" + "".join(f"E {n}\nL\nX {m}\nU\n" for n, m in (("n.txt.gz", "n.txt"), ("z.dat", "a.txt"), ("t.gz", "a.txt")))
    a, b = V.run(V.build(work / "a64b", "aarch64"), script, "aarch64"), V.run(x86, script)
    assert a == b, "aarch64 and x86_64 disagree"
    print("[PASS] DT6. detection and single-file gzip give the same output under qemu-aarch64")


MUTANTS = [
    ("zip empty record", "starts(h, n, pk56, 4)", "false"),
    ("gzip is a tar", "(arc_tar_checksum_ok(pk.blk) ||", "(false ||"),
    ("full block required", "if (pk.n == 512 && (", "if (("),
    ("zero block of tgz", "(by_ext == tar_type && all_zero(pk.blk, 512))", "false"),
    ("tar by checksum", "if (n >= 512 && h[0] && arc_tar_checksum_ok(h)) return ARC_TAR;", ""),
    ("bzip2 block magic", "!__builtin_memcmp(h + 4, bz, sizeof bz)", "true"),
    ("rar magic", "{6, {'R', 'a', 'r', '!', 0x1A, 0x07}},", ""),
    ("detect nonblocking", "O_RDONLY | O_CLOEXEC | O_NONBLOCK, 0);   /* a FIFO must not block the pane */", "O_RDONLY | O_CLOEXEC, 0);"),
    ("source nonblocking", "O_RDONLY | O_CLOEXEC | O_NONBLOCK, 0);   /* a FIFO named like an archive must not block */", "O_RDONLY | O_CLOEXEC, 0);"),
    ("gz suffix", "name[n - k] = 0;", "name[n] = 0;"),
    ("gz out suffix", 'if (!cut) (void)str_cat(name, ".out", sizeof name);', ""),
    ("empty gz", "(!res.produced && s->type != ARC_GZ && s->type != ARC_ZST)", "!res.produced"),
]
MUT_SRC = ["src/fs/arc_detect.c", "src/fs/arc_source.c"]


def test_mutants(work: Path) -> None:
    srcs = {f: (ROOT / f).read_text() for f in MUT_SRC}
    for name, old, _ in MUTANTS:
        assert any(old in t for t in srcs.values()), f"mutation anchor vanished: {name}"
    d = work / "mm"
    d.mkdir()
    tar = tar_bytes(ENTRIES)
    zbuf = io.BytesIO()
    zipfile.ZipFile(zbuf, "w").close()
    files = {"z.dat": zip_bytes(ENTRIES), "ez.dat": zbuf.getvalue(), "t.bin": tar, "g.dat": gzip.compress(tar, 6, mtime=0), "plain.gz": gzip.compress(TEXT, 6, mtime=MT),
             "plain.dat": gzip.compress(TEXT, 6, mtime=0), "UP.GZ": gzip.compress(b"x", mtime=0), "e.gz": gzip.compress(b"", mtime=0), "zeros.tgz": gzip.compress(bytes(1024)), "tiny.tgz": gzip.compress(b"short"), "z100.tgz": gzip.compress(bytes(100)),
             "x.dat": b"\xfd7zXZ\x00" + bytes(50), "r.dat": b"Rar!\x1a\x07\x01\x00" + bytes(50), "b.dat": b"BZh9 text only\n" * 3, "bz.dat": b"BZh91AY&SY" + bytes(40)}
    for n, b in files.items():
        (d / n).write_bytes(b)
    os.mkfifo(d / "p.tar")
    os.mkfifo(d / "p.dat")
    script = f"N {d}\n" + "".join(f"E {n}\nX a.txt\nX notes.txt\nX plain.dat.out\nX UP\nX e\nX zeros\nU\n" for n in sorted([*files, "p.tar", "p.dat"]))
    base = V.run(V.build(work / "mbase"), script, timeout=30)
    survivors: list[str] = []
    saved = V.NEW
    V.NEW = [x for x in V.NEW if x not in MUT_SRC]                    # the mutant's own copies replace them
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
                same = V.run(V.build(md, vfs=[*V.VFS, *extra]), script, timeout=30) == base
            except (subprocess.CalledProcessError, subprocess.TimeoutExpired, AssertionError):
                same = False                                               # a hang (FIFO) or a crash is noticed too
            if same:
                survivors.append(name)
        return go

    try:
        H.parallel([job(i, *m) for i, m in enumerate(MUTANTS)])
    finally:
        V.NEW = saved
        V.VFS = ["src/fs/archive_vfs.c", "src/fs/arc_source.c", "src/fs/arc_extract.c"]
    assert not survivors, f"mutants not noticed: {sorted(survivors)}"
    print(f"[PASS] DT7. {len(MUTANTS)}/{len(MUTANTS)} mutants of the detection and single-gzip code are noticed")


def main() -> None:
    with H.workdir() as t:
        w = Path(t).resolve()
        exe = V.build(w / "x86")
        test_renamed(exe, w)
        test_plain_gz(exe, w)
        test_unsupported(exe, w)
        test_not_blocking(exe, w)
        test_unpack(w)
        test_aarch64(exe, w)
        test_mutants(w)
    print("ALL C ARC DETECT TESTS PASSED.")


if __name__ == "__main__":
    sys.exit(main())
