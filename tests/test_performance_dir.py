#!/usr/bin/env python3
"""TDD verification suite for Duop large-directory performance architecture."""

import os
import re
import select
import shutil
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from test_pty import PtySession  # noqa: E402

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BINARY = os.path.join(REPO_ROOT, "duop")


def wait_for(sess: PtySession, predicate, timeout: float = 3.0, desc: str = "condition") -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        sess.drain(0.03)
        if predicate():
            return
    raise AssertionError(f"Timeout waiting for {desc}\n--- SCREEN ---\n{sess.vt.full_text()}")


def test_compact_structures_and_pal_contracts() -> None:
    types_inc = "".join(open(os.path.join(REPO_ROOT, f), encoding="utf-8").read() for f in ("include/types.inc", "include/types_core.inc"))
    classes_inc = open(os.path.join(REPO_ROOT, "include/classes.inc"), encoding="utf-8").read()
    config_inc = open(os.path.join(REPO_ROOT, "include/config.inc"), encoding="utf-8").read()
    pal_inc = open(os.path.join(REPO_ROOT, "include/pal.inc"), encoding="utf-8").read()
    makefile = open(os.path.join(REPO_ROOT, "Makefile"), encoding="utf-8").read()

    assert "FileEntry.name_off:" in types_inc, "FileEntry must use FileEntry.name_off into arena pool"
    assert "FileEntry.name:" not in types_inc, "FileEntry must not embed 256B FileEntry.name"
    assert "FileEntry (32 bytes" in types_inc, "FileEntry should be compact 32 bytes"

    for fld in (
        "DirModel.order,",
        "DirModel.name_pool,",
        "DirModel.raw_count,",
        "DirModel.is_loading,",
        "DirModel.huge_mode,",
        "DirModel.is_unsorted,",
    ):
        assert fld in classes_inc, f"Missing {fld} in DirModel"

    m_buf = re.search(r"\.equ\s+DIRENTS_BUF_SIZE,\s+(\d+)", config_inc)
    assert m_buf and int(m_buf.group(1)) >= 1048576, "DIRENTS_BUF_SIZE must be >= 1 MiB (1048576)"
    assert "HUGE_DIR_THRESHOLD" in config_inc, "Missing HUGE_DIR_THRESHOLD in config.inc"
    assert "FE_STAT_READY" in config_inc, "Missing FE_STAT_READY flag in config.inc"
    assert "PAL_DT_DIR" in pal_inc, "Missing PAL_DT_* normalized dirent types in pal.inc"
    assert "src/fs/dir_sort.c" in makefile and "src/fs/dir_sort_radix.c" in makefile, "Makefile must compile src/fs/dir_sort.c + dir_sort_radix.c"
    assert os.path.isfile(os.path.join(REPO_ROOT, "src/os/linux/x86_64/pal_uring.s")), "Missing pal_uring.s"
    print("[PASS] P1. Compact 32B FileEntry, name_pool, 4B order indices, 1 MiB dirent buf & PAL contracts")


def test_lazy_stat_and_visible_refresh() -> None:
    """Opening a 1500-file dir with SORT_NAME only stats visible rows, not all 1500 entries."""
    with tempfile.TemporaryDirectory() as tmp:
        left_dir = os.path.join(tmp, "many")
        right_dir = os.path.join(tmp, "empty")
        os.makedirs(left_dir)
        os.makedirs(right_dir)
        dfd = os.open(left_dir, os.O_RDONLY)
        try:
            for i in range(1500):
                fd = os.open(f"entry_{i:04d}.txt", os.O_CREAT | os.O_WRONLY, 0o644, dir_fd=dfd)
                if i < 40 or i >= 1495:
                    os.write(fd, b"A" * (100 + i))
                os.close(fd)
            os.symlink("entry_0000.txt", "link_file", dir_fd=dfd)
        finally:
            os.close(dfd)

        # 1) Measure syscall count when strace is present
        if shutil.which("strace"):
            traced_py = os.path.join(tmp, "run_trace.py")
            with open(traced_py, "w", encoding="utf-8") as f:
                f.write(
                    "import os, pty, fcntl, termios, struct, select, time\n"
                    "m, s = pty.openpty()\n"
                    "fcntl.ioctl(s, termios.TIOCSWINSZ, struct.pack('HHHH', 35, 120, 0, 0))\n"
                    "pid = os.fork()\n"
                    "if pid == 0:\n"
                    "  os.setsid(); fcntl.ioctl(s, termios.TIOCSCTTY, 0)\n"
                    "  os.dup2(s, 0); os.dup2(s, 1); os.dup2(s, 2); os.close(m); os.close(s)\n"
                    f"  os.execv({BINARY!r}, [{BINARY!r}, '--lang', 'en', {left_dir!r}, {right_dir!r}])\n"
                    "os.close(s); buf = b''; end = time.monotonic() + 5.0\n"
                    "while time.monotonic() < end:\n"
                    "  r, _, _ = select.select([m], [], [], 0.05)\n"
                    "  if r: buf += os.read(m, 65536)\n"
                    "  if b'entry_0000.txt' in buf: break\n"
                    "time.sleep(0.05); os.write(m, b'q'); os.waitpid(pid, 0); os.close(m)\n"
                )
            res = subprocess.run(
                ["strace", "-f", "-e", "trace=newfstatat,statx", sys.executable, traced_py],
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
            )
            stat_calls = sum(
                1 for ln in res.stderr.splitlines()
                if "entry_" in ln and ("newfstatat(" in ln or "statx(" in ln)
            )
            assert stat_calls < 120, f"Expected lazy stat only on visible rows (<120), saw {stat_calls} stat calls!"

        # 2) Verify visible sizes, End scroll lazy-stat, and Size-sort batch stat accuracy
        sess = PtySession(BINARY, ["--lang", "en", left_dir, right_dir], cols=120, rows=35)
        try:
            wait_for(sess, lambda: "entry_0000.txt" in sess.vt.full_text(), desc="initial render")
            wait_for(sess, lambda: "1501 files" in sess.vt.full_text(), desc="footer total count")
            # Visible row entry_0001.txt has 101 bytes ("101 B") -> visible row MUST have lazy-stat size!
            assert "101 B" in sess.vt.full_text(), f"Visible row missing lazy-stat size 101 B:\n{sess.vt.full_text()}"

            # Press End -> jumps to entry_1499.txt (1599 bytes -> "1.5K" or "1599 B"), triggers lazy stat for bottom window!
            sess.send(b"\x1b[F", wait=0.15)
            wait_for(sess, lambda: "entry_1499.txt" in sess.vt.full_text(), desc="End scroll")
            assert "1.5 K" in sess.vt.full_text() or "1599 B" in sess.vt.full_text(), (
                f"Scrolled bottom row missing lazy-stat size:\n{sess.vt.full_text()}"
            )

            # Click "Size" header twice -> descends by size across ALL 1501 files, placing entry_1499.txt (~1599 B) at top!
            pos = sess.vt.find_text("Size")
            assert pos is not None
            sx, sy = pos
            sess.send_mouse(0, sx + 1, sy, "M")
            sess.send_mouse(0, sx + 1, sy, "m")
            sess.send_mouse(0, sx + 1, sy, "M")
            sess.send_mouse(0, sx + 1, sy, "m")
            wait_for(sess, lambda: "Size ▼" in sess.vt.row_text(sy), desc="Size descending indicator")
            assert "entry_1499.txt" in sess.vt.full_text(), f"Largest file missing after batch stat sort:\n{sess.vt.full_text()}"
            # First file row under '..' (row 4 is '..') should be entry_1499.txt at row 5
            row5 = sess.vt.row_text(5)
            assert "entry_1499.txt" in row5, f"Expected largest file at top row 5, got {row5!r}"

            sess.send(b"q", wait=0.1)
            assert sess.wait_exit() == 0
        finally:
            if sess.proc_alive() if hasattr(sess, "proc_alive") else False:
                pass
    print("[PASS] P2. d_type zero-stat open (<120 stats for 1500 files), scroll lazy-stat & batch stat Size sort")


def test_sorting_modes_and_inram_filter() -> None:
    """Verify Radix/Introsort correctness (Name/Size/Mtime/Ext + tie-breakers + UTF-8) & fast RAM filter."""
    with tempfile.TemporaryDirectory() as tmp:
        left_dir = os.path.join(tmp, "sort")
        right_dir = os.path.join(tmp, "empty")
        os.makedirs(left_dir)
        os.makedirs(right_dir)
        os.makedirs(os.path.join(left_dir, "z_dir"))
        files = [
            ("b_big.z", 500, 1700000100),
            ("a_small.a", 500, 1700000050),
            ("c_mid.a", 250, 1700000200),
            ("Тест_укр.md", 100, 1700000010),
        ]
        for name, sz, mt in files:
            p = os.path.join(left_dir, name)
            with open(p, "wb") as f:
                f.write(b"a" * sz)
            os.utime(p, (mt, mt))

        sess = PtySession(BINARY, ["--lang", "en", left_dir, right_dir], cols=120, rows=35)
        try:
            wait_for(sess, lambda: "z_dir" in sess.vt.full_text(), desc="initial list")
            # Default Name sort: '..' at row 4, 'z_dir' at row 5 (dir pinned first), 'a_small.a' at row 6
            assert "z_dir" in sess.vt.row_text(5), f"Directory should precede files: {sess.vt.row_text(5)!r}"
            assert "a_small.a" in sess.vt.row_text(6), f"Name sort order wrong: {sess.vt.row_text(6)!r}"

            # Sort by Size asc -> Тест_укр(100) row 6, c_mid(250) row 7, tied 500B tie-breaks a_small < b_big
            sx, sy = sess.vt.find_text("Size")
            sess.send_mouse(0, sx + 1, sy, "M")
            sess.send_mouse(0, sx + 1, sy, "m")
            wait_for(sess, lambda: "Size ▲" in sess.vt.row_text(sy), desc="Size asc")
            assert "Тест_укр.md" in sess.vt.row_text(6), f"Size asc row 6 wrong: {sess.vt.row_text(6)!r}"
            assert "c_mid.a" in sess.vt.row_text(7), f"Size asc row 7 wrong: {sess.vt.row_text(7)!r}"
            assert "a_small.a" in sess.vt.row_text(8), f"Size tie-break row 8 wrong: {sess.vt.row_text(8)!r}"

            # Sort by Modified desc -> c_mid (1700000200) is newest file right after z_dir!
            mx, my = sess.vt.find_text("Modified")
            sess.send_mouse(0, mx + 1, my, "M")
            sess.send_mouse(0, mx + 1, my, "m")
            sess.send_mouse(0, mx + 1, my, "M")
            sess.send_mouse(0, mx + 1, my, "m")
            wait_for(sess, lambda: "Modified ▼" in sess.vt.row_text(my), desc="Modified desc")
            assert "c_mid.a" in sess.vt.row_text(6), f"Modified desc newest file wrong: {sess.vt.row_text(6)!r}"

            # In-RAM Filter '/' -> "small" -> hides b_big, keeps a_small; Esc restores all
            sess.send(b"/small\r", wait=0.1)
            wait_for(sess, lambda: "a_small.a" in sess.vt.full_text() and "b_big.z" not in sess.vt.full_text(), desc="filter")
            sess.send(b"\x1b", wait=0.1)
            wait_for(sess, lambda: "b_big.z" in sess.vt.full_text(), desc="filter clear restore")

            sess.send(b"q", wait=0.1)
            assert sess.wait_exit() == 0
        finally:
            pass
    print("[PASS] P3. Introsort + LSD Radix Sort (tie-breakers, UTF-8) & instantaneous in-RAM filter")


def send_esc_on_first_frame(sess: PtySession, timeout: float = 3.0) -> None:
    """Write Esc the instant alt-screen/first-frame arrives without waiting 50ms in drain()."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        r, _, _ = select.select([sess.master], [], [], 0.01)
        if r:
            data = os.read(sess.master, 65536)
            if data:
                sess.vt.feed(data)
            if sess.vt.saw_alt_enter:
                os.write(sess.master, b"\x1b")
                return
    raise AssertionError(f"Timeout waiting for first frame before Esc\n{sess.vt.full_text()}")


def test_huge_dir_mode_and_streaming_esc_en_ua() -> None:
    """Verify huge-dir unsorted ls -f mode, explicit sort trigger, & streaming Esc cancel (EN + UA)."""
    base = "/dev/shm" if os.path.isdir("/dev/shm") else "/tmp"
    file_total = 60000
    with tempfile.TemporaryDirectory(dir=base) as tmp:
        left_dir = os.path.join(tmp, "huge")
        right_dir = os.path.join(tmp, "empty")
        os.makedirs(left_dir)
        os.makedirs(right_dir)
        dfd = os.open(left_dir, os.O_RDONLY)
        try:
            for i in range(file_total):
                os.close(os.open(f"hf_{i:05d}.dat", os.O_CREAT | os.O_WRONLY, 0o644, dir_fd=dfd))
        finally:
            os.close(dfd)

        # 1) Huge-dir mode (DUOP_HUGE_THRESHOLD=1000) loads fast without sorting until user clicks Name header
        sess = PtySession(
            BINARY,
            ["--lang", "en", left_dir, right_dir],
            cols=120,
            rows=35,
            extra_env={"DUOP_HUGE_THRESHOLD": "1000", "DUOP_STREAM_BATCH": "4096"},
        )
        wait_for(sess, lambda: f"{file_total} files" in sess.vt.full_text(), timeout=5.0, desc="huge load finish")
        assert "[FS-order]" in sess.vt.full_text(), f"Missing EN huge-mode badge [FS-order]:\n{sess.vt.full_text()}"
        assert "Name ▲" not in sess.vt.row_text(2)[:60], f"Unsorted huge dir left pane must not display sort arrow yet: {sess.vt.row_text(2)!r}"

        # Clicking Name explicitly triggers sort and clears [FS-order] badge!
        nx, ny = sess.vt.find_text("Name")
        sess.send_mouse(0, nx + 1, ny, "M")
        sess.send_mouse(0, nx + 1, ny, "m")
        wait_for(sess, lambda: "Name ▲" in sess.vt.row_text(ny)[:60], desc="explicit huge dir sort")
        assert "[FS-order]" not in sess.vt.full_text(), "Expected [FS-order] cleared after explicit sort"
        assert "hf_00000.dat" in sess.vt.row_text(5), f"First sorted file expected at row 5, got {sess.vt.row_text(5)!r}"

        # Switch to UA and verify Ukrainian huge-mode badge on fresh reload (Ctrl+R)
        sess.send(b"\x1bl", wait=0.1)  # Alt+L -> UA
        sess.send(b"\x12", wait=0.15)  # Ctrl+R reload
        wait_for(sess, lambda: "[Порядок ФС]" in sess.vt.full_text(), timeout=5.0, desc="UA [Порядок ФС] badge")
        sess.send(b"q", wait=0.1)
        assert sess.wait_exit() == 0

        # 2) Streaming cancellation on Esc (EN)
        sess_abort = PtySession(
            BINARY,
            ["--lang", "en", left_dir, right_dir],
            cols=120,
            rows=35,
            extra_env={"DUOP_HUGE_THRESHOLD": "1000", "DUOP_STREAM_BATCH": "1"},
        )
        send_esc_on_first_frame(sess_abort)
        wait_for(
            sess_abort,
            lambda: "[Esc:stopped]" in sess_abort.vt.full_text() or "Directory streaming stopped by Esc" in sess_abort.vt.full_text(),
            timeout=3.0,
            desc="EN streaming Esc abort indicator",
        )
        assert f"{file_total} files" not in sess_abort.vt.full_text(), "Esc did not stop streaming before loading all files"
        sess_abort.send(b"q", wait=0.1)
        assert sess_abort.wait_exit() == 0

        # 3) Streaming cancellation on Esc (UA)
        sess_abort_ua = PtySession(
            BINARY,
            ["--lang", "ua", left_dir, right_dir],
            cols=120,
            rows=35,
            extra_env={"DUOP_HUGE_THRESHOLD": "1000", "DUOP_STREAM_BATCH": "1"},
        )
        send_esc_on_first_frame(sess_abort_ua)
        wait_for(
            sess_abort_ua,
            lambda: "[Зупинено Esc]" in sess_abort_ua.vt.full_text() or "Дочитування теки зупинено клавішею Esc" in sess_abort_ua.vt.full_text(),
            timeout=3.0,
            desc="UA streaming Esc abort indicator",
        )
        assert f"{file_total} files" not in sess_abort_ua.vt.full_text(), "UA Esc did not stop streaming before loading all files"
        sess_abort_ua.send(b"q", wait=0.1)
        assert sess_abort_ua.wait_exit() == 0

    print("[PASS] P4. Huge-dir [FS-order]/[Порядок ФС] mode, explicit sort & streaming Esc cancel (EN/UA)")


def main() -> None:
    assert os.path.exists(BINARY), f"Binary not found: {BINARY}"
    test_compact_structures_and_pal_contracts()
    test_lazy_stat_and_visible_refresh()
    test_sorting_modes_and_inram_filter()
    test_huge_dir_mode_and_streaming_esc_en_ua()
    print("ALL PERFORMANCE ARCHITECTURE & LARGE-DIR TESTS PASSED.")


if __name__ == "__main__":
    main()
