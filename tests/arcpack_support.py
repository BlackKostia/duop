"""Shared parts of the archive packing tests (tests/test_c_arc_pack.py): the harness build / run helpers, the source tree, and the checks of what an
archive must hold - read back with Python's tarfile / zipfile and with tar, unzip and 7z."""

import hashlib
import io
import os
import random
import shutil
import stat
import subprocess
import tarfile
import time
import zipfile
from pathlib import Path

import c_harness as H

ROOT = H.ROOT
MT = 1_600_000_000
SRCS = ["tests/harness/arcpack_cli.c", "src/fs/arc_pack.c", "src/fs/arc_pack_out.c", "src/fs/tar_write.c", "src/fs/zip_write.c", "src/fs/deflate.c", "src/fs/deflate_huff.c",
        "src/fs/task.c", "src/fs/task_conflict.c", "src/fs/file_ops_overwrite.c", "src/fs/file_ops.c", "src/fs/file_ops_delete.c", "src/fs/file_ops_copy.c",
        "src/fs/file_ops_read.c", "src/core/string.c", "src/core/string_path.c", "src/core/utf8.c", "src/core/memory.c", "src/core/syscall.c", "src/core/try_rt.c",
        "src/core/mem_prims.c", "src/core/tmpfile.c", "src/core/hash.c", "build/i18n_en.c"]
TAR, TGZ, ZIP = 0, 1, 2
FIXED = 1


def build(work: Path, arch: str = "x86_64", srcs: list[str] | None = None, cflags: list[str] | None = None) -> Path:
    work.mkdir(parents=True, exist_ok=True)
    inc = ["-I", str(ROOT / f"include/os/linux/{arch}"), "-I", str(ROOT / "include")]
    stub = work / "stubs.c"
    stub.write_text('#include "duop_task.h"\n#include "duop_tui.h"\nconst LangStrings *i18n_get(void) { return &lang_en_table; }\n'
                    'int g_task_parent_pid; TaskShared *g_task_shm;\n')
    uring = work / "pal_uring.o"
    subprocess.run([*H.ARCHS[arch]["as_"], *inc, str(ROOT / f"src/os/linux/{arch}/pal_uring.s"), "-o", str(uring)], check=True, capture_output=True)
    return H.build(arch, work, list(dict.fromkeys([*(srcs or SRCS), str(stub)])), [uring], cflags=["-I", str(ROOT / "tests/harness"), "-I", str(ROOT / "build"), *(cflags or [])])


def run(exe: Path, lines: list[str], arch: str = "x86_64", timeout: int = 300) -> list[dict]:
    res = H.run(arch, exe, [], ("\n".join(lines) + "\n").encode(), timeout=timeout)
    assert res.returncode == 0, res.stderr[-300:]
    out = []
    for ln in res.stdout.decode(errors="backslashreplace").splitlines():
        out.append({k: int(v) for k, v in (t.split("=") for t in ln.split()[1:])} if ln.startswith("P ") else {"line": ln})
    return out


def P(dst: Path, fmt: int, level: int, base: Path, names: list[str], flags: int = 0, mtime: int = MT) -> str:
    return "\t".join(["P", str(dst), str(fmt), str(level), str(flags), str(mtime), str(base), *names])


def make_tree(root: Path) -> None:
    """src/ with files of every size, modes and times, names that need PAX / UTF-8, folders, links of every kind and a fifo."""
    rnd = random.Random(0xA11)
    s = root / "src"
    s.mkdir()
    files = {"small.txt": b"hello\n", "empty.bin": b"", "run.sh": b"#!/bin/sh\necho hi\n", "secret.txt": b"top secret\n", "readonly.dat": b"ro\n",
             "unicode-ім'я-ñ.txt": "привіт\n".encode(), "with space.txt": b"spaced\n", "deep/a/b/c/d.txt": b"deep\n", "text.txt": (b"lorem ipsum dolor sit amet\n" * 20000),
             "random.bin": rnd.randbytes(300000), "mixed.bin": rnd.randbytes(100000) + bytes(900000) + rnd.randbytes(50000)}
    long = "/".join(["x" * 90, "y" * 90, "z" * 90])
    files[f"long/{long}/file.txt"] = b"long path\n"
    for name, data in files.items():
        p = s / name
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(data)
    (s / "emptydir").mkdir()
    os.chmod(s / "run.sh", 0o755)
    os.chmod(s / "secret.txt", 0o600)
    os.chmod(s / "readonly.dat", 0o444)
    os.symlink("small.txt", s / "link_rel")
    os.symlink("deep", s / "link_dir")
    os.symlink("nowhere", s / "dangling")
    os.symlink("/etc/passwd", s / "link_abs")
    os.mkfifo(s / "fifo")
    for p in [*s.rglob("*")]:
        if not p.is_symlink():
            os.utime(p, (MT, MT + (hash(p.name) % 1000)))


def expected(root: Path, base: Path, fixed: bool = False) -> dict[str, tuple]:
    """{archive name: (kind, mode, size, sha256 or link target)} of what packing `root` must store (a fifo is not stored)."""
    out: dict[str, tuple] = {}
    for p in sorted([root, *root.rglob("*")]):
        st = p.lstat()
        name = p.relative_to(base).as_posix()
        if stat.S_ISDIR(st.st_mode):
            out[name] = ("dir", 0o755 if fixed else stat.S_IMODE(st.st_mode), 0, "")
        elif stat.S_ISLNK(st.st_mode):
            out[name] = ("link", 0o777 if not fixed else 0o777, 0, os.readlink(p))
        elif stat.S_ISREG(st.st_mode):
            mode = (0o755 if st.st_mode & 0o111 else 0o644) if fixed else stat.S_IMODE(st.st_mode)
            out[name] = ("file", mode, st.st_size, hashlib.sha256(p.read_bytes()).hexdigest())
    return out


def read_tar(path: Path) -> dict[str, tuple]:
    out: dict[str, tuple] = {}
    with tarfile.open(path) as tf:
        for m in tf:
            if m.isdir():
                out[m.name] = ("dir", m.mode & 0o7777, 0, "")
            elif m.issym():
                out[m.name] = ("link", m.mode & 0o7777, 0, m.linkname)
            elif m.isreg():
                out[m.name] = ("file", m.mode & 0o7777, m.size, hashlib.sha256(tf.extractfile(m).read()).hexdigest())
    return out


def read_zip(path: Path) -> dict[str, tuple]:
    out: dict[str, tuple] = {}
    with zipfile.ZipFile(path) as zf:
        assert zf.testzip() is None
        for i in zf.infolist():
            mode = (i.external_attr >> 16) & 0o177777
            name = i.filename.rstrip("/")
            if i.is_dir():
                out[name] = ("dir", stat.S_IMODE(mode), 0, "")
            elif stat.S_ISLNK(mode):
                out[name] = ("link", stat.S_IMODE(mode), 0, zf.read(i).decode())
            else:
                out[name] = ("file", stat.S_IMODE(mode), i.file_size, hashlib.sha256(zf.read(i)).hexdigest())
    return out


def read_archive(path: Path, fmt: int) -> dict[str, tuple]:
    return read_zip(path) if fmt == ZIP else read_tar(path)


def tools_accept(path: Path, fmt: int) -> list[str]:
    """The tests the system's own tools make of the archive; the names of the tools that ran (all of them passed)."""
    ran = []
    if fmt != ZIP and shutil.which("tar"):
        subprocess.run(["tar", "tf", str(path)], check=True, capture_output=True)
        ran.append("tar")
    if fmt == ZIP and shutil.which("unzip"):
        subprocess.run(["unzip", "-tq", str(path)], check=True, capture_output=True)
        ran.append("unzip")
    if shutil.which("7z"):
        subprocess.run(["7z", "t", "-bd", str(path)], check=True, capture_output=True)
        ran.append("7z")
    return ran


def residue(d: Path) -> list[str]:
    return sorted(p.name for p in d.iterdir() if "duop-part" in p.name)
