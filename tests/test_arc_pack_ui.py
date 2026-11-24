#!/usr/bin/env python3
"""Packing from the UI (docs/ARCHIVE_ROADMAP_2026-11-24.md, phase 6): Alt+F5 asks for the archive to create (the other pane's folder and a name made from the
selection prefilled; the extension picks the format), runs the TaskWorker with the progress dialog and shows the result, on real processes through a pty.

AK1 marked entries and a folder: Alt+F5, Enter -> <other pane>/<name>.tar.gz holds exactly them (names relative to the pane), the status says it was created
AK2 one entry under the cursor gives its own name as the default; the name can be edited: ".zip" makes a zip
AK3 an archive that exists is not replaced: "already exists", the file is untouched;  AK4 an unknown extension: the message lists the formats, nothing is made
AK5 inside an archive pane: the read-only message;  AK6 on ".." and with nothing to pack: a message, no dialog
"""

import os
import sys
import tarfile
import tempfile
import time
import zipfile
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from archive_fixtures import Expect, zip_bytes  # noqa: E402
from test_arc_unpack_ui import Case, wait_for, wait_until  # noqa: E402

DOWN, UP, ENTER, ESC, BACKSPACE = b"\x1b[B", b"\x1b[A", b"\r", b"\x1b", b"\x7f"
ALT_F5 = b"\x1b[15;3~"
FILES = {"a.txt": b"alpha\n", "b.txt": b"beta\n" * 100, "sub/c.txt": b"gamma\n", "sub/d.bin": bytes(range(256)) * 40}


def case_with_tree() -> Case:
    c = Case({})
    for name, data in FILES.items():
        p = Path(c.left, name)
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(data)
    c.sess.send(b"\x12", wait=0.4)                                  # Ctrl+R: reload both panes
    return c


def test_marked_and_folder():
    c = case_with_tree()
    try:
        # sorted: .., sub, a.txt, b.txt
        c.keys(DOWN, b" ", b" ", wait=0.12)                        # mark sub, then a.txt
        c.keys(ALT_F5, wait=0.5)
        wait_for(c.sess, "Pack")
        assert os.path.join(c.right, "") in c.sess.vt.full_text() and ".tar.gz" in c.sess.vt.full_text(), "the other pane's folder and a name must be prefilled"
        c.keys(ENTER, wait=0.4)
        archives = [f for f in os.listdir(c.right) if f.endswith(".tar.gz")]
        wait_until(lambda: any(f.endswith(".tar.gz") for f in os.listdir(c.right)), "the archive appears")
        archives = [f for f in os.listdir(c.right) if f.endswith(".tar.gz")]
        wait_for(c.sess, "Archive created")
        with tarfile.open(Path(c.right, archives[0])) as tf:
            got = {m.name: (m.isdir(), tf.extractfile(m).read() if m.isfile() else None) for m in tf}
        assert got == {"sub": (True, None), "sub/c.txt": (False, FILES["sub/c.txt"]), "sub/d.bin": (False, FILES["sub/d.bin"]), "a.txt": (False, FILES["a.txt"])}, sorted(got)
        assert not any("duop-part" in f for f in os.listdir(c.right)), os.listdir(c.right)
    finally:
        c.close()
    print("[PASS] AK1. marked entries and a folder: Alt+F5 + Enter makes a .tar.gz with exactly them, the status says so")


def test_name_and_format():
    c = case_with_tree()
    try:
        c.keys(DOWN, wait=0.12)                                    # sub
        c.keys(ALT_F5, wait=0.5)
        wait_for(c.sess, "sub.tar.gz")
        c.keys(*([BACKSPACE] * 7), b".zip", wait=0.1)
        c.keys(ENTER, wait=0.4)
        wait_until(lambda: os.path.exists(os.path.join(c.right, "sub.zip")), "sub.zip appears")
        wait_for(c.sess, "Archive created")
        with zipfile.ZipFile(Path(c.right, "sub.zip")) as zf:
            assert zf.testzip() is None and sorted(zf.namelist()) == ["sub/", "sub/c.txt", "sub/d.bin"], zf.namelist()
            assert zf.read("sub/c.txt") == FILES["sub/c.txt"]
    finally:
        c.close()
    print("[PASS] AK2. the default name comes from the entry; editing the extension to .zip makes a zip")


def test_exists_and_unknown():
    c = case_with_tree()
    try:
        Path(c.right, "sub.tar.gz").write_bytes(b"precious")
        c.keys(DOWN, wait=0.12)
        c.keys(ALT_F5, wait=0.5)
        c.keys(ENTER, wait=0.4)
        wait_for(c.sess, "already exists")
        assert Path(c.right, "sub.tar.gz").read_bytes() == b"precious", "an existing archive must not be replaced"
        c.keys(ESC, wait=0.1)
        c.keys(ALT_F5, wait=0.5)
        c.keys(*([BACKSPACE] * 7), b".rar", wait=0.1)
        c.keys(ENTER, wait=0.4)
        wait_for(c.sess, ".tar.gz")
        assert sorted(os.listdir(c.right)) == ["sub.tar.gz"], os.listdir(c.right)
    finally:
        c.close()
    print("[PASS] AK3/AK4. an existing archive is kept ('already exists'); an unknown extension names the formats and makes nothing")


def test_refusals():
    c = Case({"a.zip": zip_bytes([Expect("x.txt", data=b"x")])})
    try:
        c.keys(ALT_F5, wait=0.4)                                   # on ".."
        wait_for(c.sess, "Nothing to pack")
        c.keys(DOWN, ENTER, wait=0.4)                              # into a.zip
        wait_for(c.sess, "x.txt")
        c.keys(ALT_F5, wait=0.4)
        wait_for(c.sess, "read-only")
    finally:
        c.close()
    print("[PASS] AK5/AK6. '..' gives 'Nothing to pack', an archive pane gives the read-only message; no dialog opens")


def main():
    test_marked_and_folder()
    test_name_and_format()
    test_exists_and_unknown()
    test_refusals()
    print("ALL ARCHIVE PACK UI TESTS PASSED.")


if __name__ == "__main__":
    main()
