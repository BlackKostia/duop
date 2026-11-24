#!/usr/bin/env python3
"""src/fs/arc_pack.c, tar_write.c, zip_write.c (+ deflate.c): creating .tar, .tar.gz and .zip archives (docs/ARCHIVE_ROADMAP_2026-11-24.md, phase 6).

AP1 a tree with every kind of file (empty, big, random, UTF-8 / long / spaced names, modes, links, an empty folder, a fifo) packed at several levels
    reads back with tarfile / zipfile as exactly that tree (kinds, modes, sizes, SHA-256, link targets) and passes tar / unzip / 7z
AP2 reproducible: with ARC_PACK_FIXED two runs give the same bytes whatever the times and owners; names are sorted
AP3 safety: links are stored, not followed; the archive is not packed into itself; an unreadable file, a missing name and a fifo are counted as skipped
    and the archive is still valid
AP4 atomic output: an existing destination is never replaced (-EEXIST), a missing folder is -ENOENT, nothing named .duop-part is left behind, and a
    killed run leaves nothing under the real name
AP5 ZIP64 records (forced by a small threshold) read back; tar headers for sizes beyond 8 GiB, big uid, long names and links are right
AP6 aarch64 (qemu-user) gives the x86_64 bytes;  AP7 mutation check
"""

import calendar
import os
import shutil
import signal
import subprocess
import sys
import tarfile
import time
import zipfile
from pathlib import Path

import arcpack_support as S
import c_harness as H


def pack(exe: Path, tmp: Path, fmt: int, level: int, names: list[str], out: str, flags: int = 0, base: Path | None = None) -> tuple[Path, dict]:
    dst = tmp / out
    r = S.run(exe, [S.P(dst, fmt, level, base or tmp, names, flags)])[0]
    return dst, r


def check_times(dst: Path, fmt: int, tmp: Path) -> None:
    """The modification times and owners of the entries are the files' own."""
    if fmt == S.ZIP:
        with zipfile.ZipFile(dst) as zf:
            for i in zf.infolist():
                t = (tmp / i.filename.rstrip("/")).lstat().st_mtime
                assert abs(calendar.timegm(i.date_time + (0, 0, 0)) - t) <= 2, (i.filename, i.date_time, t)
        return
    with tarfile.open(dst) as tf:
        for m in tf:
            st = (tmp / m.name).lstat()
            assert m.mtime == int(st.st_mtime) and m.uid == st.st_uid and m.gid == st.st_gid, (m.name, m.mtime, st.st_mtime)


def test_round_trip(exe: Path, work: Path) -> None:
    tmp = work / "rt"
    tmp.mkdir()
    S.make_tree(tmp)
    want = S.expected(tmp / "src", tmp)
    n = 0
    tools: set[str] = set()
    for fmt, ext in ((S.TAR, "tar"), (S.TGZ, "tar.gz"), (S.ZIP, "zip")):
        for level in (0, 1, 6, 9):
            dst, r = pack(exe, tmp, fmt, level, ["src"], f"a{fmt}_{level}.{ext}")
            assert r["rc"] == 0 and r["skipped"] == 1 and r["bytes"] == sum(v[2] for v in want.values()), (fmt, level, r)
            assert S.read_archive(dst, fmt) == want, (fmt, level)
            check_times(dst, fmt, tmp)
            tools |= set(S.tools_accept(dst, fmt))
            assert S.residue(tmp) == [], S.residue(tmp)
            n += 1
    sizes = {lv: (tmp / f"a1_{lv}.tar.gz").stat().st_size for lv in (0, 1, 6, 9)}
    assert sizes[1] < sizes[0] and sizes[9] <= sizes[1], sizes
    print(f"[PASS] AP1. {n} archives (tar / tar.gz / zip, levels 0-9): the tree reads back exactly; accepted by {', '.join(sorted(tools)) or 'Python only'}")


def test_reproducible(exe: Path, work: Path) -> None:
    tmp = work / "rep"
    tmp.mkdir()
    S.make_tree(tmp)
    outs = {}
    for fmt, ext in ((S.TAR, "tar"), (S.TGZ, "tar.gz"), (S.ZIP, "zip")):
        a, ra = pack(exe, tmp, fmt, 6, ["src"], f"a.{ext}", S.FIXED)
        for p in (tmp / "src").rglob("*"):
            if not p.is_symlink():
                os.utime(p, (S.MT + 12345, S.MT + 777))                       # other times, same contents
        b, rb = pack(exe, tmp, fmt, 6, ["src"], f"b.{ext}", S.FIXED)
        assert ra["rc"] == rb["rc"] == 0 and a.read_bytes() == b.read_bytes(), f"{ext}: two runs differ"
        outs[ext] = a
    with tarfile.open(outs["tar"]) as tf:
        ms = list(tf)
        assert [m.name for m in ms] == sorted(m.name for m in ms), "entries are in sorted order"
        assert all(m.uid == 0 and m.gid == 0 and m.uname == "" and m.mtime == S.MT for m in ms), "FIXED must zero owners and times"
        assert all(m.mode in (0o755, 0o644, 0o777) for m in ms), {m.mode for m in ms}
    with zipfile.ZipFile(outs["zip"]) as zf:
        assert all(i.date_time == (2020, 9, 13, 12, 26, 40) for i in zf.infolist()), "FIXED must fix the times of a zip"
    print("[PASS] AP2. with ARC_PACK_FIXED two runs give identical tar, tar.gz and zip bytes; owners are zero, times fixed, modes normalised")


def test_safety(exe: Path, work: Path) -> None:
    tmp = work / "safe"
    tmp.mkdir()
    S.make_tree(tmp)
    s = tmp / "src"
    os.chmod(s / "secret.txt", 0)
    r = S.run(exe, [S.P(s / "inside.tar", S.TAR, 0, tmp, ["src", "missing"])])[0]
    os.chmod(s / "secret.txt", 0o600)
    assert r["rc"] == 0 and r["skipped"] == 3 and r["err"] in (-13, -2, -6), r             # secret.txt (EACCES), the fifo, "missing" (ENOENT)
    with tarfile.open(s / "inside.tar") as tf:
        names = tf.getnames()
        assert "src/inside.tar" not in names and "src/secret.txt" not in names and "src/link_abs" in names, "the archive must not hold itself"
        assert tf.getmember("src/link_abs").linkname == "/etc/passwd" and tf.getmember("src/link_dir").issym(), "links are stored, not followed"
        assert not any(n.startswith("src/link_dir/") for n in names), "a link to a folder must not be walked"
    z = S.run(exe, [S.P(s / "inside.zip", S.ZIP, 6, tmp, ["src"])])[0]
    assert z["rc"] == 0 and "src/inside.zip" not in zipfile.ZipFile(s / "inside.zip").namelist() and "src/inside.tar" in zipfile.ZipFile(s / "inside.zip").namelist()
    print("[PASS] AP3. links stored as links, the archive is not packed into itself, unreadable / special / missing names are skipped and the archive is valid")


def test_atomic(exe: Path, work: Path) -> None:
    tmp = work / "atom"
    tmp.mkdir()
    S.make_tree(tmp)
    dst = tmp / "keep.tar"
    dst.write_bytes(b"precious")
    r = S.run(exe, [S.P(dst, S.TAR, 0, tmp, ["src"])])[0]
    assert r["rc"] == -17 and dst.read_bytes() == b"precious" and S.residue(tmp) == [], (r, S.residue(tmp))
    r = S.run(exe, [S.P(tmp / "nodir" / "x.tar", S.TAR, 0, tmp, ["src"])])[0]
    assert r["rc"] == -2 and not (tmp / "nodir").exists(), r
    big = tmp / "big"
    big.mkdir()
    for i in range(40):
        (big / f"f{i}.bin").write_bytes(os.urandom(4 << 20))
    out = tmp / "killed.tar.gz"
    p = subprocess.Popen([*H.ARCHS["x86_64"]["run"], str(exe)], stdin=subprocess.PIPE, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    p.stdin.write((S.P(out, S.TGZ, 9, tmp, ["big"]) + "\n").encode())
    p.stdin.close()
    time.sleep(0.4)
    os.kill(p.pid, signal.SIGKILL)
    p.wait()
    assert not out.exists(), "a killed run must not leave the archive under its real name"
    shutil.rmtree(big)
    for leftover in tmp.glob("*duop-part*"):
        leftover.unlink()
    print("[PASS] AP4. an existing destination is kept (-EEXIST), a missing folder is -ENOENT, a killed run leaves nothing under the real name")


def test_zip64_and_tar_headers(work: Path) -> None:
    tmp = work / "z64"
    tmp.mkdir()
    S.make_tree(tmp)
    exe = S.build(work / "z64b", cflags=["-DARC_PACK_ZIP64_AT=1000ull"])
    dst = tmp / "z.zip"
    r = S.run(exe, [S.P(dst, S.ZIP, 6, tmp, ["src"])])[0]
    assert r["rc"] == 0, r
    got = S.read_zip(dst)
    want = S.expected(tmp / "src", tmp)
    assert got == want, "ZIP64 entries must read back"
    S.tools_accept(dst, S.ZIP)
    hdrs = S.run(exe, ["\t".join(["H", str(9 << 30), "3000000", str(S.MT), "0", "420", "big.bin", ""]),
                       "\t".join(["H", "5", "1000", "-1", "2", "511", "l", "t" * 150]),
                       "\t".join(["H", "0", "0", "99999999999", "0", "420", "d" * 70 + "/" + "e" * 100 + "/f", ""])])
    import io
    for line, (size, uid, mt, name, kind) in zip(hdrs, ((9 << 30, 3000000, S.MT, "big.bin", "reg"), (0, 1000, -1, "l", "sym"), (0, 0, 99999999999, "d" * 70 + "/" + "e" * 100 + "/f", "reg"))):
        raw = bytes.fromhex(line["line"][2:])
        tf = tarfile.open(fileobj=io.BytesIO(raw + bytes(1024)), mode="r:")
        m = tf.next()
        assert m.name == name and m.size == size and m.uid == uid and m.mtime == mt, (name, m.name, m.size, m.uid, m.mtime)
        assert (kind == "sym") == m.issym() and (kind != "sym" or m.linkname == "t" * 150), m.linkname
    print("[PASS] AP5. ZIP64 entries (data descriptors, 64-bit sizes) read back; tar headers carry sizes over 8 GiB, big uids, long names / links and odd times through PAX")


def test_aarch64(x86: Path, work: Path) -> None:
    if not H.available("aarch64"):
        print("[SKIP] AP6. aarch64 toolchain / qemu-user missing")
        return
    tmp = work / "a64"
    tmp.mkdir()
    S.make_tree(tmp)
    a64 = S.build(work / "a64b", "aarch64")
    for fmt, ext in ((S.TAR, "tar"), (S.TGZ, "tar.gz"), (S.ZIP, "zip")):
        a, _ = pack(x86, tmp, fmt, 6, ["src"], f"x.{ext}", S.FIXED)
        r = S.run(a64, [S.P(tmp / f"y.{ext}", fmt, 6, tmp, ["src"], S.FIXED)], "aarch64")[0]
        assert r["rc"] == 0 and a.read_bytes() == (tmp / f"y.{ext}").read_bytes(), ext
    print("[PASS] AP6. tar, tar.gz and zip are the same bytes under qemu-aarch64")


def main() -> None:
    with H.workdir() as t:
        w = Path(t).resolve()
        exe = S.build(w / "x86")
        test_round_trip(exe, w)
        test_reproducible(exe, w)
        test_safety(exe, w)
        test_atomic(exe, w)
        test_zip64_and_tar_headers(w)
        test_aarch64(exe, w)
        from pack_mut import test_mutants
        test_mutants(w)
    print("ALL C ARC PACK TESTS PASSED.")


if __name__ == "__main__":
    sys.exit(main())
