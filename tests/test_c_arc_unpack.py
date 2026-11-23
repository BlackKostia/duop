#!/usr/bin/env python3
"""src/fs/arc_unpack*.c, arc_path.c: unpacking and testing archives (docs/ARCHIVE_TOOLS_PLAN.md, phase 2), on the engine the TaskWorker runs.

UP1 every fixture archive unpacks to exactly the model tree: file bytes (SHA-256), modes (umask applied, no setuid), symlinks, folders,
    modification times; a hard link becomes a copy of its target
UP2 selections: files and folders, a folder inside a folder, overlapping selections (no double work), smart unpack (a subfolder for
    several top-level entries, none for a single one)
UP3 safety: '..' and absolute names, an escaping or absolute symlink, a symlink already in the destination (folder and file), a hard link to
    a path outside the archive: nothing is created or touched outside the destination, the offenders are counted as skipped
UP4 conflicts: Overwrite, Skip, Rename for files; a folder where a file belongs and the reverse are skipped
UP5 damage: a flipped byte (CRC), an encrypted member, a bzip2 / lzma member and a torn tar skip only the member; no temporary file is left
UP6 limits: max_bytes and the decompression-bomb ratio refuse before anything is written
UP7 a full disk / file size limit stops the unpack with the error and leaves no temporary file;  UP8 SIGKILL: no partial file under a real name
UP9 arc_test: counts good members and reports damaged ones without writing;  UP10 aarch64 equals x86_64;  UP11 mutation check
"""

import hashlib
import io
import os
import random
import resource
import signal
import stat
import subprocess
import sys
import tarfile
import tempfile
import time
import zipfile
import zlib
from pathlib import Path

import c_harness as H
from archive_fixtures import MTIME, Expect, build_corpus, payload, tar_bytes, zip_bytes
from arcunp_support import (EFBIG, OVERWRITE, RENAME, ROOT, SKIP, SRCS_ENGINE, UMASK, U, actual, build, expected, kv, make_evil, no_residue, norm, prep_evil_dst,
                            run, same_tree)

def test_fixtures(exe: Path, work: Path) -> None:
    fx = build_corpus(work / "corpus", many=2000)
    names = [n for n, f in fx.items() if f.fmt in ("tar", "tgz", "zip") and f.entries and n not in ("tree_bzip2.zip", "tree_lzma.zip")]
    script = ""
    for n in names:
        (work / "o" / n).mkdir(parents=True)
        script += U(fx[n].path, work / "o" / n)
    out = run(exe, script)
    for n, line in zip(names, out):
        r = kv(line)
        assert r["rc"] == 0 and r["skipped"] == 0, (n, line)
        want = expected(fx[n].entries, zip_mode=n.endswith(".zip"))
        same_tree(actual(work / "o" / n), want, n)
        no_residue(work / "o" / n, n)
        for e in fx[n].entries:
            if e.kind == "file":
                p = work / "o" / n / norm(e.name)
                assert int(p.stat().st_mtime) == MTIME, f"{n}: {e.name} mtime {p.stat().st_mtime}"
        assert r["files"] == sum(1 for e in fx[n].entries if e.kind in ("file", "hardlink")), (n, line)
        assert r["links"] == sum(1 for e in fx[n].entries if e.kind == "symlink"), (n, line)
        for e in fx[n].entries:
            if e.kind == "dir":
                assert int((work / "o" / n / norm(e.name)).stat().st_mtime) == MTIME, f"{n}: folder {e.name} lost its time"
    print(f"[PASS] UP1. {len(names)} fixture archives unpack to exactly the model tree (bytes, modes, symlinks, folders, mtimes)")


def test_selection(exe: Path, work: Path) -> None:
    fx = build_corpus(work / "corpus")
    ents = fx["tree.zip"].entries
    cases = [("a", "", ("readme.txt", "docs")), ("b", "docs/", ("guide",)), ("c", "", ("docs", "docs/guide", "readme.txt", "readme.txt")), ("d", "", ("nothing-here",))]
    script = ""
    for tag, folder, members in cases:
        (work / "sel" / tag).mkdir(parents=True)
        script += U(fx["tree.zip"].path, work / "sel" / tag, folder=folder or "-", members=members)
    out = run(exe, script)
    for (tag, folder, members), line in zip(cases, out):
        r = kv(line)
        assert r["rc"] == 0 and r["kept"] == 0, (tag, line)
        got = actual(work / "sel" / tag)
        if tag == "c":
            want = expected(ents, ("docs", "readme.txt"), "")
        elif tag == "d":
            want = {}
        else:
            want = expected(ents, tuple(m.split("/")[0] for m in members), folder)
        same_tree(got, want, tag)
        assert kv(line)["files"] == sum(1 for v in want.values() if v[0] == "file"), (tag, line)       # an overlapping selection is not done twice
    fresh = work / "sel" / "fresh" / "new"
    (work / "sel" / "fresh").mkdir()
    assert kv(run(exe, U(fx["tree.zip"].path, fresh, members=("readme.txt",)))[0])["rc"] == 0 and (fresh / "readme.txt").exists(), "a missing destination is created"
    single = work / "single.zip"
    single.write_bytes(zip_bytes([Expect("proj/", "dir"), Expect("proj/a.txt", data=b"a"), Expect("proj/sub/b.txt", data=b"b")]))
    lone = work / "lone.zip"
    lone.write_bytes(zip_bytes([Expect("only.txt", data=b"o")]))
    multi = fx["tree.zip"].path
    for d in ("s1", "s2", "s3", "s4"):
        (work / "smart" / d).mkdir(parents=True)
    out = run(exe, U(single, work / "smart/s1", smart=1) + U(lone, work / "smart/s2", smart=1) + U(multi, work / "smart/s3", smart=1) + U(multi, work / "smart/s4", smart=0))
    assert all(kv(l)["rc"] == 0 for l in out), out
    assert sorted(os.listdir(work / "smart/s1")) == ["proj"], "a single top-level folder needs no extra folder"
    assert sorted(os.listdir(work / "smart/s2")) == ["only.txt"], "a single top-level file needs no extra folder"
    assert sorted(os.listdir(work / "smart/s3")) == ["tree"] and (work / "smart/s3/tree/readme.txt").read_bytes() == b"hello archive\n", "several top-level entries go into <name>/"
    assert "readme.txt" in os.listdir(work / "smart/s4"), "without smart unpack the members land in the destination itself"
    print("[PASS] UP2. selections of files / folders / sub-folders, overlapping selections, smart unpack")


def test_safety(exe: Path, work: Path) -> None:
    base = work / "safe"
    arc, outside, victim = make_evil(base)
    dst = base / "dst"
    prep_evil_dst(dst, outside, victim)
    out = run(exe, U(arc, dst))
    r = kv(out[0])
    assert r["rc"] == 0, out
    assert (outside / "sentinel.txt").read_text() == "untouched" and sorted(os.listdir(outside)) == ["sentinel.txt"], f"files appeared outside: {os.listdir(outside)}"
    assert victim.read_text() == "victim", "a symlink in the destination must be replaced, never written through"
    assert not (base / "escape.txt").exists() and not (base / "escape2.txt").exists(), "'..' member escaped"
    got = actual(dst)
    assert got["ok.txt"][0] == "file" and got["abs/file.txt"][0] == "file", sorted(got)
    assert (dst / "ok.txt").stat().st_mode & 0o7000 == 0, "setuid / setgid / sticky must be dropped"
    assert got["lnok"] == ("symlink", "ok.txt", 0) and got["lnabs"][0] == "dir", got.get("lnabs")   # the absolute link was skipped; lnabs/pwn.txt made a real folder
    assert "lnup" not in got and "hl-abs" not in got and "dev" not in got and "fifo" not in got, sorted(got)
    assert got["hl-ok"][0] == "file" and got["hl-ok"][1] == got["ok.txt"][1], "a hard link is a copy of its target member"
    assert got["victimlink"][0] == "file" and (dst / "victimlink").read_bytes() == b"overwrites the link, not its target"
    assert (dst / "preexisting").is_symlink() and not (outside / "pwn.txt").exists(), "must not descend into a symlink that is already there"
    assert r["skipped"] >= 8, out[0]
    no_residue(dst, "safety")
    z = base / "evil.zip"
    with zipfile.ZipFile(z, "w") as zf:
        zf.writestr("fine.txt", b"fine")
        zf.writestr("../zip-escape.txt", b"x")
        zf.writestr("x/../../zip-escape2.txt", b"x")
    zd = base / "zdst"
    zd.mkdir()
    r2 = kv(run(exe, U(z, zd))[0])
    assert sorted(os.listdir(zd)) == ["fine.txt"] and r2["skipped"] == 2 and not (base / "zip-escape.txt").exists(), (os.listdir(zd), r2)
    print(f"[PASS] UP3. '..', absolute names, escaping / absolute symlinks, existing symlinks, hard links to outside, devices: nothing outside the destination, {r['skipped']} members skipped")


def test_conflicts(exe: Path, work: Path) -> None:
    fx = build_corpus(work / "corpus")
    arc = fx["tree.zip"].path
    res = {}
    for pol in (OVERWRITE, SKIP, RENAME):
        d = work / "cf" / str(pol)
        d.mkdir(parents=True)
        (d / "readme.txt").write_text("old content")
        (d / "docs").write_text("a file where the archive has a folder")
        (d / "bin").mkdir()
        (d / "bin" / "tool").mkdir()                         # a folder where the archive has a file
        res[pol] = (d, kv(run(exe, U(arc, d, policy=pol))[0]))
    d, r = res[OVERWRITE]
    assert (d / "readme.txt").read_bytes() == b"hello archive\n" and r["rc"] == 0, r
    d, r = res[SKIP]
    assert (d / "readme.txt").read_text() == "old content" and r["kept"] >= 1, r
    d, r = res[RENAME]
    assert (d / "readme.txt").read_text() == "old content" and (d / "readme_1.txt").read_bytes() == b"hello archive\n", sorted(os.listdir(d))
    for pol in (OVERWRITE, SKIP, RENAME):
        d, r = res[pol]
        assert (d / "docs").is_file() and (d / "bin" / "tool").is_dir() and r["skipped"] >= 2, (pol, r)       # kind mismatches are never replaced
        no_residue(d, f"conflict {pol}")
    print("[PASS] UP4. Overwrite / Skip / Rename for files; a folder where a file belongs and the reverse are skipped and counted")


def test_damage(exe: Path, work: Path) -> None:
    base = work / "dmg"
    base.mkdir()
    ents = [Expect("a.txt", data=b"A" * 3000), Expect("b.txt", data=payload(7, 6000)), Expect("c.txt", data=b"c")]
    z = bytearray(zip_bytes(ents, zipfile.ZIP_STORED))
    z[z.index(b"AAAA") + 5] ^= 0xFF
    (base / "crc.zip").write_bytes(bytes(z))
    (base / "cut.tar").write_bytes(tar_bytes(ents)[:512 + 3072 + 512 + 6144 - 100])
    fx = build_corpus(base / "c")
    script = ""
    for n, a in (("crc", base / "crc.zip"), ("cut", base / "cut.tar"), ("lzma", fx["tree_lzma.zip"].path), ("enc", fx["encrypted.zip"].path)):
        (base / n).mkdir()
        script += U(a, base / n)
    out = run(exe, script)
    crc, cut, lz, en = (kv(l) for l in out)
    assert crc["rc"] == 0 and crc["skipped"] == 1 and crc["err"] == -74 and crc["files"] == 2, crc
    assert sorted(os.listdir(base / "crc")) == ["b.txt", "c.txt"] and (base / "crc" / "b.txt").read_bytes() == ents[1].data, os.listdir(base / "crc")
    assert cut["rc"] == 0 and (base / "cut" / "a.txt").read_bytes() == ents[0].data and cut["files"] >= 1, cut
    assert lz["skipped"] == 10 and lz["err"] == -95 and lz["files"] == 0, lz            # every file member of tree_lzma.zip uses method 14
    assert en["skipped"] == 2 and en["err"] == -13, en
    for n in ("crc", "cut", "lzma", "enc"):
        no_residue(base / n, n)
    print("[PASS] UP5. a flipped byte, a torn tar, lzma and encrypted members: only the member is skipped, nothing partial is left")


def test_limits(exe: Path, work: Path) -> None:
    base = work / "lim"
    base.mkdir()
    zeros = base / "zeros.zip"
    zeros.write_bytes(zip_bytes([Expect("z.bin", data=bytes(4 * 1024 * 1024)), Expect("tiny", data=b"t")], zipfile.ZIP_DEFLATED))
    for n in ("bomb", "cap", "ok"):
        (base / n).mkdir()
    out = run(exe, U(zeros, base / "bomb", floor=1 << 20, ratio=100) + U(zeros, base / "cap", cap=1 << 20) + U(zeros, base / "ok", floor=1 << 20, ratio=100000, cap=1 << 30))
    assert kv(out[0])["rc"] == EFBIG and kv(out[1])["rc"] == EFBIG and kv(out[2])["rc"] == 0, out
    assert os.listdir(base / "bomb") == [] and os.listdir(base / "cap") == [], "a refused archive must write nothing"
    print("[PASS] UP6. the byte cap and the decompression-bomb ratio refuse before anything is written")


def test_write_failure(exe: Path, work: Path) -> None:
    base = work / "fsz"
    base.mkdir()
    arc = base / "a.zip"
    arc.write_bytes(zip_bytes([Expect("a_small.txt", data=b"s" * 100), Expect("z_big.bin", data=payload(9, 1 << 20))], zipfile.ZIP_STORED))
    (base / "d").mkdir()
    r = kv(run(exe, U(arc, base / "d"), limit_fsize=64 * 1024)[0])
    assert r["rc"] < 0, r
    assert (base / "d" / "a_small.txt").read_bytes() == b"s" * 100 and not (base / "d" / "z_big.bin").exists(), os.listdir(base / "d")
    no_residue(base / "d", "write failure")
    print(f"[PASS] UP7. a write failing mid-file (file size limit) stops the unpack with the error (rc={r['rc']}); finished files stay, no partial file, no temporary file")


def test_sigkill(exe: Path, work: Path) -> None:
    base = work / "kill"
    base.mkdir()
    ents = [Expect(f"f{i}.bin", data=payload(40 + i, 24 * 1024 * 1024)) for i in range(5)]
    arc = base / "big.zip"
    arc.write_bytes(zip_bytes(ents, zipfile.ZIP_STORED))
    (base / "d").mkdir()
    p = subprocess.Popen([str(exe)], stdin=subprocess.PIPE, stdout=subprocess.DEVNULL)
    p.stdin.write(U(arc, base / "d").encode())
    p.stdin.close()
    deadline = time.monotonic() + 20
    while time.monotonic() < deadline and not any((base / "d").iterdir()):
        time.sleep(0.01)
    time.sleep(0.15)
    p.kill()
    p.wait()
    done = 0
    for e in ents:
        f = base / "d" / e.name
        if f.exists():
            assert f.read_bytes() == e.data, f"{e.name} exists under its real name but is not complete"
            done += 1
    assert done < len(ents), "the unpack finished before it was killed: the test proves nothing"
    print(f"[PASS] UP8. SIGKILL in the middle: {done} of {len(ents)} files finished, every file under a real name is complete (only *.duop-part* may remain)")


def test_arc_test(exe: Path, work: Path) -> None:
    base = work / "tst"
    base.mkdir()
    ents = [Expect("a.txt", data=b"A" * 3000), Expect("b.txt", data=payload(7, 6000)), Expect("d/", "dir")]
    good, bad = base / "good.zip", base / "bad.zip"
    good.write_bytes(zip_bytes(ents))
    z = bytearray(zip_bytes(ents, zipfile.ZIP_STORED))
    z[z.index(b"AAAA") + 5] ^= 0xFF
    bad.write_bytes(bytes(z))
    tgz = base / "g.tar.gz"
    import gzip
    tgz.write_bytes(gzip.compress(tar_bytes(ents), 6, mtime=0))
    junk = base / "junk.zip"
    junk.write_bytes(random.Random(1).randbytes(2000))
    out = run(exe, f"T {good}\nT {bad}\nT {tgz}\nT {junk}\n")
    g, b, t, j = (kv(l) for l in out)
    assert g == {"rc": 0, "files": 2, "bytes": 9000, "skipped": 0, "err": 0, "dmg": 0}, g
    assert b["rc"] == 0 and b["skipped"] == 1 and b["err"] == -74 and b["files"] == 1, b
    assert t == {"rc": 0, "files": 2, "bytes": 9000, "skipped": 0, "err": 0, "dmg": 0}, t
    assert j["rc"] < 0, j
    print("[PASS] UP9. Test Archive counts good members, reports a bad CRC without writing, handles tar.gz, refuses non-archives")


def test_aarch64(x86: Path, work: Path) -> None:
    if not H.available("aarch64"):
        print("[SKIP] UP10. aarch64 toolchain / qemu-user missing")
        return
    fx = build_corpus(work / "corpus")
    names = ("tree.zip", "tree.tgz", "links.tar", "tree_gnu.tar")
    res = []
    for arch, exe in (("x86_64", x86), ("aarch64", build(work / "a64", "aarch64"))):
        script = ""
        for n in names:
            d = work / "arch" / arch / n
            d.mkdir(parents=True)
            script += U(fx[n].path, d)
        out = run(exe, script, arch)
        res.append(([kv(l) for l in out], [actual(work / "arch" / arch / n) for n in names]))
    assert res[0] == res[1], "aarch64 and x86_64 unpack differently"
    print("[PASS] UP10. four archives unpack identically under qemu-aarch64")


# Equivalent, not listed: the EISDIR test in put_file (rename over a folder fails and skips the member just the same), the fsync and the
# RENAME_NOREPLACE flag (no test can race a file into existence), the 0700 of folder modes (archived folders are 0755), the hop limit of hard links.
MUTANTS = [
    ("nofollow", "O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC, 0);\n        if (fd == -ENOENT && create)", "O_RDONLY | O_DIRECTORY | O_CLOEXEC, 0);\n        if (fd == -ENOENT && create)"),
    ("link climbs", "if (--depth < 0) return false;", "--depth;"),
    ("absolute link", "if (!target[0] || target[0] == '/') return false;", "if (!target[0]) return false;"),
    ("setuid kept", "uint32_t mode = meta->mode ? meta->mode & 0777 : 0644;", "uint32_t mode = meta->mode ? meta->mode : 0644;"),
    ("kept counter", "if (act == CONFLICT_ACT_SKIP) {\n            u->r->kept++;", "if (act == CONFLICT_ACT_SKIP) {"),
    ("mtime", "if (rc == 0) stamp((int)fd, nullptr, meta->mtime, 0);", ""),
    ("part removed", "(void)os_unlinkat(pfd, part, 0);\n        return x.sink_err", "return x.sink_err"),
    ("unsafe link made", "if (!arc_link_safe(rel, target)) return note(u, -EPERM);", "if (!arc_link_safe(rel, target) && !target[0]) return note(u, -EPERM);"),
    ("tar link target", "    if (l) {\n        (void)str_copy(target, l, PATH_MAX);", "    if (false) {\n        (void)str_copy(target, l, PATH_MAX);"),
    ("link counter", "u->r->links++;", ""),
    ("folder times", "    arc_put_dir_times(u);\n    return 0;", "    return 0;"),
    ("selection dedupe", "if (s->done[item >> 3] & (1u << (item & 7))) return 0;", ""),
    ("smart single", "count_top(&u->idx) > 1", "count_top(&u->idx) > 0"),
    ("byte cap", "if (o->max_bytes && bytes > o->max_bytes) return -EFBIG;", ""),
    ("bomb", "if (o->bomb_ratio && bytes > o->bomb_floor && bytes / (archive_size ? archive_size : 1) > o->bomb_ratio) return -EFBIG;", "if (false && o->bomb_ratio && bytes > o->bomb_floor && bytes / (archive_size ? archive_size : 1) > o->bomb_ratio) return -EFBIG;"),
    ("folder prefix", "u->selpfx = str_len(names);", "u->selpfx = 0;"),
    ("create destination", "intptr_t m = os_mkdirat(AT_FDCWD, dst, 0755);\n    if (m < 0 && m != -EEXIST) return m;", "intptr_t m = -ENOENT;\n    if (m < 0 && m != -EEXIST) return m;"),
    ("damaged flag", "if (u->info.damaged) r->damaged = 1;\n    mem_free(s.sel);", "mem_free(s.sel);"),
]


def test_mutants(work: Path) -> None:
    srcs = {s: (ROOT / s).read_text() for s in SRCS_ENGINE if s.startswith(("src/fs/arc_unpack", "src/fs/arc_path"))}
    for name, old, _ in MUTANTS:
        assert any(old in t for t in srcs.values()), f"mutation anchor vanished: {name}"
    fx = build_corpus(work / "mc")
    base = work / "mm"
    ents = [Expect("a.txt", data=b"A" * 3000), Expect("b.txt", data=payload(7, 6000)), Expect("c.txt", data=b"c")]
    z = bytearray(zip_bytes(ents, zipfile.ZIP_STORED))
    z[z.index(b"AAAA") + 5] ^= 0xFF
    (base / "crc.zip").parent.mkdir(parents=True, exist_ok=True)
    (base / "crc.zip").write_bytes(bytes(z))
    (base / "zeros.zip").write_bytes(zip_bytes([Expect("z.bin", data=bytes(4 * 1024 * 1024))], zipfile.ZIP_DEFLATED))

    def script(root: Path) -> str:
        s = ""
        for i, (a, kw) in enumerate(((fx["tree.zip"].path, {}), (fx["links.tar"].path, {}), (fx["tree.tgz"].path, {"smart": 1}), (base / "crc.zip", {}),
                                      (fx["tree.zip"].path, {"folder": "docs/", "members": ("guide",)}), (fx["tree.zip"].path, {"policy": SKIP}),
                                      (fx["tree.zip"].path, {"policy": RENAME}))):
            d = root / str(i)
            d.mkdir(parents=True)
            if i >= 5:
                (d / "readme.txt").write_text("old")
            s += U(a, d, **kw)
        ev, outside, victim = make_evil(root / "ev")
        prep_evil_dst(root / "ev" / "dst", outside, victim)
        return s + U(ev, root / "ev" / "dst") + U(fx["tree.zip"].path, root / "ev" / "fresh" / "x", members=("readme.txt",)) + U(fx["tree.zip"].path, root / "ev" / "fresh2", members=("readme.txt",)) \
            + U(fx["tree.zip"].path, root / "capd", cap=100) + U(base / "zeros.zip", root / "bombd", floor=1 << 20, ratio=100)

    def snap(root: Path) -> list:
        return sorted((str(p.relative_to(root)), p.is_symlink() and os.readlink(p) or (p.is_file() and hashlib.sha256(p.read_bytes()).hexdigest()) or "d") for p in root.rglob("*"))

    r0 = work / "mb"
    r0.mkdir()
    (r0 / "capd").mkdir()
    (r0 / "bombd").mkdir()
    base_out = run(build(work / "mbase"), script(r0))
    base_snap = snap(r0)
    survivors: list[str] = []

    def job(i: int, name: str, old: str, new: str):
        def go() -> None:
            md = work / f"m{i}"
            md.mkdir()
            eng = []
            patched = False
            for s in SRCS_ENGINE:
                if s in srcs:
                    t = srcs[s]
                    if not patched and old in t:
                        t = t.replace(old, new, 1)
                        patched = True
                    p = md / Path(s).name
                    p.write_text(t)
                    eng.append(str(p))
                else:
                    eng.append(s)
            rr = work / f"mr{i}"
            rr.mkdir()
            (rr / "capd").mkdir()
            (rr / "bombd").mkdir()
            exe = build(md, engine=eng)                                        # a mutant that does not compile is a bug of the test: let it fail loudly
            try:
                same = run(exe, script(rr), timeout=60) == base_out and snap(rr) == base_snap
            except (subprocess.TimeoutExpired, AssertionError):
                same = False                                                   # a crash or a hang is noticed too
            if same:
                survivors.append(name)
        return go

    H.parallel([job(i, *m) for i, m in enumerate(MUTANTS)])
    assert not survivors, f"mutants not noticed: {sorted(survivors)}"
    print(f"[PASS] UP11. {len(MUTANTS)}/{len(MUTANTS)} mutants of the unpack engine are noticed")


def main() -> None:
    with H.workdir() as d:
        w = Path(d).resolve()
        exe = build(w / "x86")
        test_fixtures(exe, w)
        test_selection(exe, w)
        test_safety(exe, w)
        test_conflicts(exe, w)
        test_damage(exe, w)
        test_limits(exe, w)
        test_write_failure(exe, w)
        test_sigkill(exe, w)
        test_arc_test(exe, w)
        test_aarch64(exe, w)
        test_mutants(w)
    print("ALL C ARC UNPACK TESTS PASSED.")


if __name__ == "__main__":
    sys.exit(main())
