#!/usr/bin/env python3
"""Enter on archive-like files in a real pane: browse what can be browsed, say why when not - never a silent hex dump.

AE1 a zip-based file (.docx / .jar / .apk) opens as a folder
AE2 .tar.xz / .7z / .rar / .zst / a plain .gz: a status message ("cannot be browsed yet"), no viewer
AE3 a .zip / .tar.gz that is damaged: "Not a readable archive", no viewer;  F3 still shows the raw bytes
"""

import gzip
import os
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from archive_fixtures import Expect, zip_bytes  # noqa: E402
from test_pty import PtySession  # noqa: E402

BINARY = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "duop"))
DOWN, ENTER, ESC, F3 = b"\x1b[B", b"\r", b"\x1b", b"\x1bOR"


def wait_for(sess, text, timeout=5.0):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        sess.drain(0.08)
        if text in sess.vt.full_text():
            return
    raise AssertionError(f"timed out waiting for {text!r}:\n{sess.vt.full_text()}")


def kill(sess):
    for step in (lambda: os.kill(sess.pid, 9), lambda: os.waitpid(sess.pid, 0), lambda: os.close(sess.master)):
        try:
            step()
        except OSError:
            pass


def session(files):
    tmp = tempfile.mkdtemp(prefix="aen_")
    left, right = os.path.join(tmp, "left"), os.path.join(tmp, "right")
    os.makedirs(left)
    os.makedirs(right)
    for n, d in files.items():
        Path(left, n).write_bytes(d)
    s = PtySession(BINARY, ["--lang", "en", left, right], cols=120, rows=35, extra_env={"HOME": tmp})
    s.drain(0.4)
    return s


def open_nth(sess, n):
    for _ in range(n):
        sess.send(DOWN, wait=0.1)
    sess.send(ENTER, wait=0.5)


def test_zip_based_opens_as_folder():
    z = zip_bytes([Expect("word/document.xml", data=b"<w/>"), Expect("readme.txt", data=b"hi")])
    sess = session({"a.docx": z, "b.jar": z, "c.apk": z})
    try:
        for i, name in enumerate(("a.docx", "b.jar", "c.apk")):
            if i:
                sess.send(b"\x7f", wait=0.3)
            open_nth(sess, 1 if i == 0 else 1 + i)
            wait_for(sess, "readme.txt")
            assert "word" in sess.vt.full_text(), f"{name} did not open as a folder"
    finally:
        kill(sess)
    print("[PASS] AE1. .docx / .jar / .apk open as folders")


def test_unsupported_formats_explain():
    names = ("a.7z", "b.rar", "c.tar.xz", "d.zst", "e.gz")
    sess = session({n: gzip.compress(b"x" * 500) if n == "e.gz" else b"\x00opaque\xff" * 200 for n in names})
    try:
        for i, name in enumerate(names):
            open_nth(sess, 1 if i == 0 else 1)
            wait_for(sess, "cannot be browsed yet")
            assert "Name" in sess.vt.full_text(), f"{name}: the pane must stay a pane (no viewer)"
            sess.send(ESC, wait=0.1)
    finally:
        kill(sess)
    print("[PASS] AE2. unsupported archive formats say so instead of opening a hex dump")


def test_damaged_says_so_and_f3_still_views():
    sess = session({"bad.zip": b"PK\x03\x04 this is not a real zip" * 20, "bad2.tar.gz": b"\x1f\x8b\x08\x00 broken" * 20})
    try:
        open_nth(sess, 1)
        wait_for(sess, "Not a readable archive")
        assert "Name" in sess.vt.full_text()
        sess.send(F3, wait=0.5)
        wait_for(sess, "PK")                                         # F3: the raw bytes in the viewer
    finally:
        kill(sess)
    print("[PASS] AE3. a damaged archive: 'Not a readable archive' on Enter, the raw bytes on F3")


def main():
    test_zip_based_opens_as_folder()
    test_unsupported_formats_explain()
    test_damaged_says_so_and_f3_still_views()
    print("ALL ARCHIVE ENTER UI TESTS PASSED.")


if __name__ == "__main__":
    main()
