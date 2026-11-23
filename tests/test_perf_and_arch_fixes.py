#!/usr/bin/env python3
"""TDD tests for:
1. Git $PATH resolution (no hardcoded /usr/bin/git).
2. Slab allocator 256 KiB arena reclamation (OS_MUNMAP when empty).
3. Archive VFS >4 MiB truncation warning & ZIP64 support.
"""
import os
import struct
import subprocess
import sys
import tarfile
import tempfile
import time
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from test_pty import PtySession

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
BINARY = os.path.join(ROOT, "duop")

_SLAB_HARNESS_ASM = r"""
.intel_syntax noprefix
.include "pal.inc"

.section .bss
ptrs: .space 4096 * 8

.section .text
.global _start
.extern mem_alloc
.extern mem_free
.extern mem_slab_arena_count

_start:
    # Allocate 4096 blocks of 128 bytes (~512 KiB -> >= 3 arenas)
    xor ebx, ebx
    lea r12, [rip + ptrs]
.La_loop:
    cmp ebx, 4096; jge .La_chk
    mov edi, 128
    call mem_alloc
    test rax, rax; jz .Lfail
    mov [r12 + rbx * 8], rax
    inc ebx; jmp .La_loop
.La_chk:
    call mem_slab_arena_count
    cmp eax, 2; jl .Lfail          # must have allocated multiple arenas
    mov r13d, eax

    # Free all 4096 blocks
    xor ebx, ebx
.Lf_loop:
    cmp ebx, 4096; jge .Lf_chk
    mov rdi, [r12 + rbx * 8]
    call mem_free
    inc ebx; jmp .Lf_loop
.Lf_chk:
    call mem_slab_arena_count
    cmp eax, r13d; jge .Lfail      # empty arenas must be munmap'd!
    cmp eax, 1; jg .Lfail          # at most 1 warm arena retained
    xor edi, edi; mov eax, 60; syscall
.Lfail:
    mov edi, 1; mov eax, 60; syscall
"""


def wait_for(sess, text, timeout=3.0):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        sess.drain(0.08)
        if text in sess.vt.full_text():
            return
    raise AssertionError(f"Timed out waiting for {text!r};\n{sess.vt.full_text()}")


def test_git_uses_path_not_hardcoded():
    """Duop must resolve git from $PATH instead of hardcoding /usr/bin/git."""
    with tempfile.TemporaryDirectory() as tmp:
        bin_dir = os.path.join(tmp, "custom_bin")
        repo = os.path.join(tmp, "repo")
        plain = os.path.join(tmp, "plain")
        os.makedirs(bin_dir)
        os.makedirs(repo)
        os.makedirs(plain)
        os.makedirs(os.path.join(repo, ".git"))
        with open(os.path.join(repo, "hello.txt"), "w") as f:
            f.write("hi\n")

        fake_git = os.path.join(bin_dir, "git")
        with open(fake_git, "w") as f:
            f.write("#!/bin/sh\nprintf '## nix-path-branch\\0 M hello.txt\\0'\n")
        os.chmod(fake_git, 0o755)

        old_path = os.environ.get("PATH", "")
        os.environ["PATH"] = f"{bin_dir}:{old_path}"
        try:
            sess = PtySession(BINARY, ["--lang", "en", repo, plain], cols=120, rows=30)
        finally:
            os.environ["PATH"] = old_path
        try:
            wait_for(sess, "nix-path-branch", timeout=3.0)
            sess.send(b"\x1b[21~")
            _, st = os.waitpid(sess.pid, 0)
            assert os.WIFEXITED(st) and os.WEXITSTATUS(st) == 0
        finally:
            try:
                os.kill(sess.pid, 9)
                os.waitpid(sess.pid, 0)
            except OSError:
                pass


def test_slab_arena_reclamation():
    """Freeing all blocks in 256 KiB slab arenas must munmap empty arenas."""
    with tempfile.TemporaryDirectory() as d:
        s_path = os.path.join(d, "slab.s")
        o_path = os.path.join(d, "slab.o")
        b_path = os.path.join(d, "slab")
        open(s_path, "w").write(_SLAB_HARNESS_ASM)
        subprocess.check_call([
            "as", "-I", os.path.join(ROOT, "include/os/linux/x86_64"),
            "-I", os.path.join(ROOT, "include"),
            s_path, "-o", o_path,
        ])
        subprocess.check_call([
            "ld", o_path,
            os.path.join(ROOT, "build/core/memory.o"),
            os.path.join(ROOT, "build/os/linux/x86_64/pal_os.o"),
            os.path.join(ROOT, "build/os/linux/x86_64/pal_file.o"),
            os.path.join(ROOT, "build/os/linux/x86_64/pal_sys_mem.o"),
            os.path.join(ROOT, "build/os/linux/x86_64/pal_fault.o"),
            os.path.join(ROOT, "build/os/linux/x86_64/pal_simd.o"),
            os.path.join(ROOT, "build/core/try_rt.o"),
            "-o", b_path,
        ])
        res = subprocess.run([b_path], timeout=5)
        assert res.returncode == 0, f"Slab arena reclamation failed: rc={res.returncode}"


def make_zip64(path, payload=b"hello from zip64\n"):
    name = b"z64.txt"
    crc = zlib.crc32(payload) & 0xFFFFFFFF
    lh = struct.pack(
        "<IHHHHHIIIHH",
        0x04034B50, 45, 0, 0, 0, 0, crc, len(payload), len(payload), len(name), 0,
    ) + name + payload
    cd_off = len(lh)
    extra = struct.pack("<HHQQQ", 0x0001, 24, len(payload), len(payload), 0)
    cd = struct.pack(
        "<IHHHHHHIIIHHHHHII",
        0x02014B50, 45, 45, 0, 0, 0, 0, crc, 0xFFFFFFFF, 0xFFFFFFFF,
        len(name), len(extra), 0, 0, 0, 0, 0xFFFFFFFF,
    ) + name + extra
    z64_eocd_off = cd_off + len(cd)
    z64_eocd = struct.pack(
        "<IQHHIIQQQQ",
        0x06064B50, 44, 45, 45, 0, 0, 1, 1, len(cd), cd_off,
    )
    z64_loc = struct.pack("<IIQI", 0x07064B50, 0, z64_eocd_off, 1)
    eocd = struct.pack("<IHHHHIIH", 0x06054B50, 0, 0, 0xFFFF, 0xFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0)
    with open(path, "wb") as f:
        f.write(lh + cd + z64_eocd + z64_loc + eocd)


def test_zip64_and_truncation_warning():
    """ZIP64 entries must parse/view cleanly, and a tar above the old 4 MiB limit is entered whole (no truncation warning)."""
    with tempfile.TemporaryDirectory() as tmp:
        l, r = os.path.join(tmp, "left"), os.path.join(tmp, "right")
        os.makedirs(l)
        os.makedirs(r)
        make_zip64(os.path.join(l, "a_z64.zip"))
        big_tar = os.path.join(l, "b_big.tar")
        with tarfile.open(big_tar, "w") as tf:
            import io
            data = b"A" * (4 * 1024 * 1024 + 8192)
            ti = tarfile.TarInfo("huge.bin")
            ti.size = len(data)
            tf.addfile(ti, io.BytesIO(data))

        sess = PtySession(BINARY, ["--lang", "en", l, r], cols=120, rows=30)
        try:
            wait_for(sess, "a_z64.zip")
            sess.send(b"\x1b[B")   # a_z64.zip
            sess.send(b"\r")
            wait_for(sess, "z64.txt")
            sess.send(b"\x1b[B")   # z64.txt
            sess.send(b"\x1bOR")   # F3 view
            wait_for(sess, "hello from zip64")
            sess.send(b"\x1b", wait=0.15)
            sess.send(b"\x7f", wait=0.2)
            wait_for(sess, "b_big.tar")
            sess.send(b"\x1b[B")   # b_big.tar
            sess.send(b"\r")
            wait_for(sess, "huge.bin")                         # listed although it lies beyond the old 4 MiB read limit
            assert "limit" not in sess.vt.full_text() and "damaged" not in sess.vt.full_text(), sess.vt.full_text()
            sess.send(b"\x1b[21~")
            _, st = os.waitpid(sess.pid, 0)
            assert os.WIFEXITED(st) and os.WEXITSTATUS(st) == 0
        finally:
            try:
                os.kill(sess.pid, 9)
                os.waitpid(sess.pid, 0)
            except OSError:
                pass


if __name__ == "__main__":
    for fn in [
        test_git_uses_path_not_hardcoded,
        test_slab_arena_reclamation,
        test_zip64_and_truncation_warning,
    ]:
        print(f"RUN {fn.__name__}...", end=" ", flush=True)
        fn()
        print("OK")
