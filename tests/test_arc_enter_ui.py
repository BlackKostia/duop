#!/usr/bin/env python3
"""Enter on archive-like files in a real pane: browse what can be browsed, say why when not - never a silent hex dump.

AE1 a zip-based file (.docx / .jar / .apk) opens as a folder
AE2 .tar.xz / .7z / .rar / .bz2, and an xz file under another name: a status message ("cannot be browsed yet"), no viewer
AE3 a .zip / .tar.gz that is damaged: "Not a readable archive", no viewer;  F3 still shows the raw bytes
AE5 a plain .gz opens as a one-member folder, a zip under another name opens as a folder
AE4 the same by a mouse double click: an archive opens as a folder (not a hex dump), a folder inside it and ".." work, an unsupported
    format and a damaged archive show their message
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
    names = ("a.7z", "b.rar", "c.tar.xz", "d.bz2", "e.dat")
    sess = session({n: b"\xfd7zXZ\x00" + bytes(300) if n == "e.dat" else b"\x00opaque\xff" * 200 for n in names})
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


def test_plain_gz_and_renamed_archives_open():
    z = zip_bytes([Expect("readme.txt", data=b"hi")])
    sess = session({"notes.txt.gz": gzip.compress(b"hello gzip\n" * 20), "renamed.dat": z, "text.txt": b"just text\n"})
    try:
        open_nth(sess, 1)                                            # notes.txt.gz (sorted first): its one member
        wait_for(sess, "/left/notes.txt.gz ")
        assert "« .." in sess.vt.full_text() and "notes.txt" in sess.vt.full_text(), sess.vt.full_text()
        sess.send(b"\x7f", wait=0.3)                                 # .. out of it
        wait_for(sess, "renamed.dat")
        sess.send(DOWN, wait=0.1)
        sess.send(ENTER, wait=0.5)                                   # renamed.dat: a zip under another name
        wait_for(sess, "readme.txt")
    finally:
        kill(sess)
    print("[PASS] AE5. a plain .gz opens as a one-member folder; a zip under another name opens as a folder")


def double_click(sess, row):
    """Two left clicks within the double-click time on screen row `row` of the left pane (row 0 = title bar)."""
    for _ in range(2):
        sess.send_mouse(0, 10, row, "M", wait=0.03)
        sess.send_mouse(0, 10, row, "m", wait=0.03)
    sess.drain(0.4)


def test_double_click():
    z = zip_bytes([Expect("readme.txt", data=b"hi"), Expect("docs/", "dir"), Expect("docs/inner.txt", data=b"inner")])
    sess = session({"a.zip": z, "b.7z": b"\x00opaque" * 100, "c.zip": b"PK\x03\x04 broken" * 50})
    try:
        # rows: 0 header, 1 pane title, 2 column titles, 3 divider, 4 "..", 5 a.zip, 6 b.7z, 7 c.zip
        double_click(sess, 5)
        wait_for(sess, "/left/a.zip ")                       # the pane title is the archive: inside it (the raw bytes of a hex dump also mention readme.txt)
        assert "« .." in sess.vt.full_text() and "docs" in sess.vt.full_text(), "a double click on a.zip must show its members, not a hex dump"
        double_click(sess, 5)                                # the folder docs (sorted first)
        wait_for(sess, "inner.txt")
        double_click(sess, 4)                                # ".." up to the archive root
        wait_for(sess, "readme.txt")
        double_click(sess, 4)                                # ".." out of the archive
        wait_for(sess, "b.7z")
        assert "/left/a.zip " not in sess.vt.full_text(), "'..' at the archive root must leave the archive"
        double_click(sess, 6)
        wait_for(sess, "cannot be browsed yet")
        double_click(sess, 7)
        wait_for(sess, "Not a readable archive")
        assert "Name" in sess.vt.full_text()
    finally:
        kill(sess)
    print("[PASS] AE4. mouse double click: an archive opens as a folder, folders and '..' work, unsupported / damaged archives say why")


def main():
    test_zip_based_opens_as_folder()
    test_unsupported_formats_explain()
    test_damaged_says_so_and_f3_still_views()
    test_plain_gz_and_renamed_archives_open()
    test_double_click()
    print("ALL ARCHIVE ENTER UI TESTS PASSED.")


if __name__ == "__main__":
    main()
