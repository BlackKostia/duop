"""Shared parts of the archive unpack tests (tests/test_c_arc_unpack.py): the harness build / run helpers, the model of an unpacked tree and
the evil archive that tries every trick on its destination."""

import hashlib
import io
import os
import resource
import signal
import subprocess
import tarfile
from pathlib import Path

import c_harness as H
from archive_fixtures import MTIME, Expect

ROOT = H.ROOT
SRCS_ENGINE = ["src/fs/arc_unpack.c", "src/fs/arc_unpack_item.c", "src/fs/arc_path.c", "src/fs/arc_task.c", "src/fs/arc_source.c", "src/fs/arc_detect.c",
               "src/fs/arc_extract.c", "src/fs/arc_index.c", "src/fs/arc_tar.c", "src/fs/arc_zip.c", "src/fs/gzip_dec.c", "src/fs/inflate_stream.c",
               "src/fs/archive_parse.c", "src/fs/inflate.c", "src/core/hash.c"]
SRCS_BASE = ["tests/harness/arcunpack_cli.c", "src/fs/task.c", "src/fs/task_conflict.c", "src/fs/file_ops_overwrite.c", "src/fs/file_ops.c",
             "src/fs/file_ops_delete.c", "src/fs/file_ops_copy.c", "src/fs/file_ops_read.c", "src/core/string.c", "src/core/string_path.c", "src/core/utf8.c",
             "src/core/memory.c", "src/core/syscall.c", "src/core/try_rt.c", "src/core/mem_prims.c", "src/core/tmpfile.c", "build/i18n_en.c"]
POLICY_ASK, OVERWRITE, SKIP, RENAME = 0, 1, 2, 3
EFBIG = -27
UMASK = os.umask(0)
os.umask(UMASK)


def build(work: Path, arch: str = "x86_64", engine: list[str] | None = None) -> Path:
    work.mkdir(parents=True, exist_ok=True)
    inc = ["-I", str(ROOT / f"include/os/linux/{arch}"), "-I", str(ROOT / "include")]
    stub = work / "stubs.c"
    stub.write_text('#include "duop_task.h"\n#include "duop_tui.h"\nconst LangStrings *i18n_get(void) { return &lang_en_table; }\n'
                    'int g_task_parent_pid; TaskShared *g_task_shm;\n')
    uring = work / "pal_uring.o"
    subprocess.run([*H.ARCHS[arch]["as_"], *inc, str(ROOT / f"src/os/linux/{arch}/pal_uring.s"), "-o", str(uring)], check=True, capture_output=True)
    srcs = list(dict.fromkeys([*SRCS_BASE, str(stub), *(engine or SRCS_ENGINE)]))
    return H.build(arch, work, srcs, [uring], cflags=["-I", str(ROOT / "tests/harness"), "-I", str(ROOT / "build")])


def run(exe: Path, script: str, arch: str = "x86_64", timeout: int = 300, limit_fsize: int | None = None) -> list[str]:
    def pre() -> None:
        if limit_fsize is not None:
            signal.signal(signal.SIGXFSZ, signal.SIG_IGN)
            resource.setrlimit(resource.RLIMIT_FSIZE, (limit_fsize, limit_fsize))
    res = subprocess.run([*H.ARCHS[arch]["run"], str(exe)], input=script.encode(), capture_output=True, timeout=timeout, preexec_fn=pre)
    assert res.returncode == 0, res.stderr[-300:]
    return res.stdout.decode(errors="backslashreplace").splitlines()


def U(archive: Path, dst: Path, policy: int = OVERWRITE, smart: int = 0, floor: int = 0, ratio: int = 0, cap: int = 0, folder: str = "-", members: tuple[str, ...] = ()) -> str:
    return f"U {archive} {dst} {policy} {smart} {floor} {ratio} {cap} {folder} {' '.join(members)}\n"


def kv(line: str) -> dict[str, int]:
    return {k: int(v) for k, v in (t.split("=") for t in line.split()[1:])}


def norm(name: str) -> str:
    return "/".join(c for c in name.split("/") if c not in ("", "."))


def expected(entries: list[Expect], members: tuple[str, ...] = (), folder: str = "", zip_mode: bool = False) -> dict[str, tuple]:
    """The tree an unpack of `members` (all when empty) of `folder` must leave: {path: (kind, sha-or-link, mode or None)}."""
    byname = {norm(e.name): e for e in entries if e.kind != "dir"}
    out: dict[str, tuple] = {}

    def want(e: Expect, rel: str) -> None:
        parts = rel.split("/")
        for i in range(1, len(parts)):
            out.setdefault("/".join(parts[:i]), ("dir", "", None))
        if e.kind == "dir":
            out["/".join(parts)] = ("dir", "", (e.mode | 0o700) & 0o777 & ~UMASK)
        elif e.kind == "symlink":
            out[rel] = ("symlink", e.link, 0)
        else:
            data = e.data if e.kind == "file" else b"" if zip_mode else byname[norm(e.link)].data      # a zip has no hard links: an empty file
            out[rel] = ("file", hashlib.sha256(data).hexdigest(), e.mode & 0o777 & ~UMASK)

    for e in entries:
        n = norm(e.name)
        if not n.startswith(folder):
            continue
        rel = n[len(folder):]
        top = rel.split("/")[0]
        if members and top not in members:
            continue
        if rel:
            want(e, rel)
    return out


def actual(root: Path) -> dict[str, tuple]:
    out: dict[str, tuple] = {}
    for dp, dns, fns in os.walk(root, followlinks=False):
        for n in dns + fns:
            p = Path(dp) / n
            rel = p.relative_to(root).as_posix()
            if p.is_symlink():
                out[rel] = ("symlink", os.readlink(p), 0)
            elif p.is_dir():
                out[rel] = ("dir", "", p.stat().st_mode & 0o777)
            else:
                out[rel] = ("file", hashlib.sha256(p.read_bytes()).hexdigest(), p.stat().st_mode & 0o777)
    return out


def same_tree(got: dict, want: dict, label: str) -> None:
    assert set(got) == set(want), f"{label}: only unpacked {sorted(set(got) - set(want))[:5]}, missing {sorted(set(want) - set(got))[:5]}"
    for k, (kind, val, mode) in want.items():
        g = got[k]
        assert g[0] == kind and (kind == "dir" or g[1] == val), f"{label}: {k}: {g[:2]} != {(kind, val)}"
        if mode is not None and kind != "symlink":
            assert g[2] == mode, f"{label}: {k}: mode {g[2]:o} != {mode:o}"


def no_residue(root: Path, label: str) -> None:
    left = [p for p in root.rglob("*") if "duop-part" in p.name]
    assert not left, f"{label}: temporary files left behind: {left[:3]}"


def make_evil(base: Path) -> tuple[Path, Path, Path]:
    """(archive, outside folder, victim file): every trick an archive can play on its destination."""
    outside = base / "outside"
    outside.mkdir(parents=True)
    (outside / "sentinel.txt").write_text("untouched")
    victim = base / "victim.txt"
    victim.write_text("victim")
    tb = io.BytesIO()
    with tarfile.open(fileobj=tb, mode="w", format=tarfile.GNU_FORMAT) as tf:
        def add(name, data=b"x", typ=tarfile.REGTYPE, link=""):
            ti = tarfile.TarInfo(name)
            ti.type, ti.linkname, ti.mode, ti.mtime = typ, link, 0o4755 if typ == tarfile.REGTYPE else 0o755, MTIME
            ti.size = len(data) if typ == tarfile.REGTYPE else 0
            tf.addfile(ti, io.BytesIO(data) if typ == tarfile.REGTYPE else None)
        add("ok.txt", b"ok")
        add("../escape.txt")
        add("a/../../escape2.txt")
        add("/abs/file.txt", b"abs")
        add("lnabs", typ=tarfile.SYMTYPE, link=str(outside))
        add("lnup", typ=tarfile.SYMTYPE, link="../outside")
        add("lnok", typ=tarfile.SYMTYPE, link="ok.txt")
        add("lnabs/pwn.txt", b"pwn")
        add("hl-abs", typ=tarfile.LNKTYPE, link="/etc/passwd")
        add("hl-ok", typ=tarfile.LNKTYPE, link="ok.txt")
        add("preexisting/pwn.txt", b"pwn")
        add("victimlink", b"overwrites the link, not its target")
        add("dev", typ=tarfile.CHRTYPE)
        add("fifo", typ=tarfile.FIFOTYPE)
    arc = base / "evil.tar"
    arc.write_bytes(tb.getvalue())
    return arc, outside, victim


def prep_evil_dst(dst: Path, outside: Path, victim: Path) -> None:
    dst.mkdir()
    (dst / "preexisting").symlink_to(outside)                  # symlinks that are already there
    (dst / "victimlink").symlink_to(victim)


