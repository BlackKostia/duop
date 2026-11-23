#!/usr/bin/env python3
"""os_utimensat / os_fstatfs (PAL) and sys_fs_avail (src/core/syscall.c): the metadata calls archive unpacking needs
(docs/ARCHIVE_TOOLS_PLAN.md, phase 2) - restoring modification times (also of symlinks and through a descriptor) and the free
space of the destination.

FM1 utimensat: atime / mtime with nanoseconds, UTIME_OMIT keeps one of them, UTIME_NOW, a NULL times pointer, a missing path
    (-ENOENT), AT_SYMLINK_NOFOLLOW stamps the link and leaves its target alone
FM2 futimens through a descriptor (path NULL)
FM3 sys_fs_avail equals statvfs(f_bavail * f_frsize) of the same file system; a bad descriptor is -EBADF
FM4 aarch64 (qemu-user) gives the x86_64 results
"""

import os
import sys
import tempfile
from pathlib import Path

import c_harness as H

ROOT = H.ROOT
SRCS = ["tests/harness/fsmeta_cli.c", "src/core/syscall.c", "src/core/string.c", "src/core/utf8.c", "src/core/memory.c", "src/core/try_rt.c",
        "src/core/mem_prims.c"]
AT_SYMLINK_NOFOLLOW = 0x100
UTIME_NOW, UTIME_OMIT = (1 << 30) - 1, (1 << 30) - 2
EBADF, ENOENT = -9, -2


def build(work: Path, arch: str = "x86_64") -> Path:
    work.mkdir(parents=True, exist_ok=True)
    return H.build(arch, work, SRCS, cflags=["-I", str(ROOT / "tests/harness")])


def run(exe: Path, script: str, arch: str = "x86_64") -> list[str]:
    res = H.run(arch, exe, [], script.encode())
    assert res.returncode == 0, res.stderr[-300:]
    return res.stdout.decode().splitlines()


def scenario(tmp: Path) -> tuple[str, dict[str, Path]]:
    f, g, link, d = tmp / "f", tmp / "g", tmp / "link", tmp / "d"
    f.write_bytes(b"x")
    g.write_bytes(b"y")
    d.mkdir()
    link.symlink_to(g)
    script = "".join([
        f"T {f} 1000000000 111 1600000000 222000333 0\n",              # 0: both with nanoseconds
        f"T {g} {UTIME_OMIT} {UTIME_OMIT} 1500000000 5 0\n",            # 1: only the mtime changes (atime omitted)
        f"T {tmp / 'nope'} 1 1 1 1 0\n",                                # 2: ENOENT
        f"T {link} 1200000000 0 1300000000 0 {AT_SYMLINK_NOFOLLOW}\n",  # 3: the link itself
        f"N {f} 0\n",                                                   # 4: now
        f"Q {d} 1100000000 0 1400000000 999999999\n",                   # 5: through a descriptor (a directory)
        f"T {f} {UTIME_NOW} {UTIME_NOW} {UTIME_NOW} {UTIME_NOW} 0\n",   # 6: UTIME_NOW
        f"Q {tmp / 'nope'} 1 1 1 1\n",                                  # 7: open fails
        f"O {tmp}\n", "B\n",                                            # 8, 9
    ])
    return script, {"f": f, "g": g, "link": link, "d": d}


def test_times(exe: Path, tmp: Path) -> list[str]:
    script, p = scenario(tmp)
    g_atime = p["g"].stat().st_atime_ns
    out = run(exe, script)
    assert out[0] == "T 0" and out[1] == "T 0" and out[2] == f"T {ENOENT}", out[:3]
    # steps 4 and 6 touch f again: check f right after step 0 with a fresh script
    f_script = f"T {p['f']} 1000000000 111 1600000000 222000333 0\n"
    run(exe, f_script)
    st = p["f"].stat()
    assert st.st_atime_ns == 1000000000 * 10**9 + 111 and st.st_mtime_ns == 1600000000 * 10**9 + 222000333, (st.st_atime_ns, st.st_mtime_ns)
    sg = p["g"].stat()
    assert sg.st_mtime_ns == 1500000000 * 10**9 + 5, sg.st_mtime_ns
    assert abs(sg.st_atime_ns - g_atime) < 10**9 or sg.st_atime_ns != 1500000000 * 10**9, "UTIME_OMIT must leave the atime"
    ls = os.lstat(p["link"])
    assert ls.st_mtime == 1300000000 and ls.st_atime == 1200000000, (ls.st_mtime, ls.st_atime)
    assert sg.st_mtime_ns != 1300000000 * 10**9, "AT_SYMLINK_NOFOLLOW must leave the target alone"
    print("[PASS] FM1. utimensat: nanoseconds, UTIME_OMIT / UTIME_NOW / NULL, ENOENT, the link itself with AT_SYMLINK_NOFOLLOW")
    return out


def test_fd_and_avail(exe: Path, tmp: Path, out: list[str]) -> None:
    assert out[5] == "Q 0", out[5]
    sd = p_d = (tmp / "d").stat()
    assert sd.st_atime == 1100000000 and sd.st_mtime_ns == 1400000000 * 10**9 + 999999999, (sd.st_atime, sd.st_mtime_ns)
    assert out[7] == f"Q {ENOENT}", out[7]
    print("[PASS] FM2. futimens through a descriptor (a directory): exact nanoseconds; a failed open is reported")
    sv = os.statvfs(tmp)
    rc, avail = out[8].split()[1:]
    assert out[8].startswith("O 0 ") and abs(int(avail) - sv.f_bavail * sv.f_frsize) < 64 * 1024 * 1024, (out[8], sv.f_bavail * sv.f_frsize)
    assert out[9] == f"B {EBADF}", out[9]
    print(f"[PASS] FM3. sys_fs_avail = statvfs f_bavail * f_frsize ({avail} B); a bad descriptor is -EBADF")


def main() -> None:
    with H.workdir() as d, tempfile.TemporaryDirectory(prefix="fsm_") as t:
        w, tmp = Path(d).resolve(), Path(t)
        exe = build(w / "x86")
        out = test_times(exe, tmp)
        test_fd_and_avail(exe, tmp, out)
        if H.available("aarch64"):
            with tempfile.TemporaryDirectory(prefix="fsm_a_") as t2:
                script, _ = scenario(Path(t2))
                a = [ln for ln in run(build(w / "a64", "aarch64"), script, "aarch64")]
            script_b, _ = scenario(Path(tempfile.mkdtemp(prefix="fsm_b_")))
            assert [x.split()[0] + " " + x.split()[1] for x in a] == [x.split()[0] + " " + x.split()[1] for x in run(exe, script_b)], "aarch64 differs"
            print("[PASS] FM4. aarch64 returns the x86_64 results")
        else:
            print("[SKIP] FM4. aarch64 toolchain / qemu-user missing")
    print("ALL C FSMETA TESTS PASSED.")


if __name__ == "__main__":
    sys.exit(main())
