#!/usr/bin/env python3
"""Unpacking and testing archives from the UI (docs/ARCHIVE_TOOLS_PLAN.md, phase 2): F5 in an archive pane, Alt+F9 on an archive file,
Alt+Shift+F9 (Test Archive), the progress / conflict dialogs and the result messages, on real processes through a pty.

AU1 F5 inside an archive: the Unpack dialog (the other pane's folder prefilled), Enter -> the whole folder arrives in the other pane
AU2 F5 on one entry unpacks just that entry; marked entries (Space) unpack just those; a folder entry brings its subtree
AU3 Alt+F9 on an archive file of a normal pane: smart unpack into <name>/ (several top-level entries), none for a single folder
AU4 Alt+Shift+F9: "Archive is OK" for a good archive, the count of unusable members for a damaged one; both leave the disk alone
AU5 conflicts: the File Conflict dialog appears for an existing file; Skip keeps it, Overwrite replaces it
AU6 an evil archive: nothing lands outside the destination, the status counts the skipped members
AU7 Cancel (c) in the progress dialog stops a big unpack: "Operation canceled", no temporary file, the worker is gone
AU8 the pane of the archive survives an unpack (still lists the archive, ".." works); a file that only has an archive name is reported as unreadable
"""

import gzip
import os
import sys
import tempfile
import time
import zipfile
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from archive_fixtures import Expect, payload, zip_bytes  # noqa: E402
from test_pty import PtySession  # noqa: E402

BINARY = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "duop"))
DOWN, UP, ENTER, ESC, BACKSPACE, F5 = b"\x1b[B", b"\x1b[A", b"\r", b"\x1b", b"\x7f", b"\x1b[15~"
ALT_F9, ALT_SHIFT_F9 = b"\x1b[20;3~", b"\x1b[20;4~"
TREE = [Expect("readme.txt", data=b"hello archive\n"), Expect("docs/", "dir"), Expect("docs/guide.md", data=b"# guide\n" * 100),
        Expect("docs/sub/deep.txt", data=b"deep\n"), Expect("bin.dat", data=payload(3, 20000))]


def wait_for(sess, text, timeout=6.0):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        sess.drain(0.08)
        if text in sess.vt.full_text():
            return
    raise AssertionError(f"timed out waiting for {text!r}:\n{sess.vt.full_text()}")


def wait_until(pred, desc, timeout=8.0):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        if pred():
            return
        time.sleep(0.05)
    raise AssertionError(f"timed out: {desc}")


def kill(sess):
    for step in (lambda: os.kill(sess.pid, 9), lambda: os.waitpid(sess.pid, 0), lambda: os.close(sess.master)):
        try:
            step()
        except OSError:
            pass


class Case:
    def __init__(self, files: dict[str, bytes]):
        self.tmp = tempfile.mkdtemp(prefix="auu_")
        self.left, self.right = os.path.join(self.tmp, "left"), os.path.join(self.tmp, "right")
        os.makedirs(self.left)
        os.makedirs(self.right)
        for n, d in files.items():
            Path(self.left, n).write_bytes(d)
        self.sess = PtySession(BINARY, ["--lang", "en", self.left, self.right], cols=120, rows=35, extra_env={"HOME": self.tmp})
        self.sess.drain(0.4)

    def keys(self, *ks, wait=0.15):
        for k in ks:
            self.sess.send(k, wait=wait)

    def close(self):
        kill(self.sess)

    def right_files(self) -> dict[str, bytes]:
        out = {}
        for dp, _, fns in os.walk(self.right):
            for f in fns:
                p = Path(dp, f)
                out[p.relative_to(self.right).as_posix()] = p.read_bytes()
        return out


def enter_archive(c: Case, downs: int = 1):
    c.keys(*([DOWN] * downs), ENTER, wait=0.2)
    wait_for(c.sess, "readme.txt")


def test_f5_whole_folder():
    c = Case({"a.zip": zip_bytes(TREE)})
    try:
        enter_archive(c)                                   # the cursor is on ".."
        c.keys(F5, wait=0.4)
        wait_for(c.sess, "Unpack")
        assert c.right in c.sess.vt.full_text(), "the other pane's folder must be prefilled"
        c.keys(ENTER, wait=0.2)
        wait_until(lambda: len(c.right_files()) == 4, "unpacked files")
        wait_for(c.sess, "Unpacked")
        got = c.right_files()
        assert got == {"readme.txt": b"hello archive\n", "docs/guide.md": b"# guide\n" * 100, "docs/sub/deep.txt": b"deep\n", "bin.dat": payload(3, 20000)}, sorted(got)
        wait_for(c.sess, "a.zip")                          # AU8: the archive pane is still the archive, listing its members
        c.keys(BACKSPACE, wait=0.3)
        assert "a.zip" in c.sess.vt.full_text() and "Name" in c.sess.vt.full_text()
    finally:
        c.close()
    print("[PASS] AU1. F5 in an archive pane: the Unpack dialog (destination prefilled), the whole folder arrives, the archive pane survives (AU8)")


def test_selection():
    c = Case({"a.zip": zip_bytes(TREE)})
    try:
        enter_archive(c)
        # sorted: .., then folders (docs), then files: bin.dat, readme.txt
        c.keys(DOWN, DOWN, wait=0.12)                      # bin.dat
        c.keys(F5, wait=0.4)
        wait_for(c.sess, "Unpack")
        c.keys(ENTER, wait=0.2)
        wait_until(lambda: "bin.dat" in c.right_files(), "single entry")
        assert list(c.right_files()) == ["bin.dat"], c.right_files().keys()
        wait_for(c.sess, "Unpacked")
        c.keys(UP, UP, wait=0.12)                           # docs
        c.keys(F5, wait=0.4)
        c.keys(ENTER, wait=0.3)
        wait_until(lambda: "docs/sub/deep.txt" in c.right_files(), "folder subtree")
        assert sorted(c.right_files()) == ["bin.dat", "docs/guide.md", "docs/sub/deep.txt"], sorted(c.right_files())
    finally:
        c.close()
    c = Case({"a.zip": zip_bytes(TREE)})
    try:
        enter_archive(c)
        c.keys(DOWN, DOWN, b" ", b" ", wait=0.12)          # mark bin.dat and readme.txt (Space marks and moves down)
        c.keys(F5, wait=0.4)
        c.keys(ENTER, wait=0.3)
        wait_until(lambda: len(c.right_files()) == 2, "marked entries")
        assert sorted(c.right_files()) == ["bin.dat", "readme.txt"], sorted(c.right_files())
    finally:
        c.close()
    print("[PASS] AU2. one entry, a folder with its subtree, marked entries")


def test_alt_f9_smart():
    multi = zip_bytes(TREE)
    single = zip_bytes([Expect("proj/", "dir"), Expect("proj/a.txt", data=b"a"), Expect("proj/sub/b.txt", data=b"b")])
    c = Case({"multi.zip": multi, "proj.zip": single})
    try:
        c.keys(DOWN, wait=0.15)                              # multi.zip
        c.keys(ALT_F9, wait=0.4)
        wait_for(c.sess, "Unpack")
        c.keys(ENTER, wait=0.3)
        wait_until(lambda: "multi/readme.txt" in c.right_files(), "smart folder")
        assert sorted(c.right_files()) == ["multi/bin.dat", "multi/docs/guide.md", "multi/docs/sub/deep.txt", "multi/readme.txt"], sorted(c.right_files())
        c.keys(DOWN, wait=0.15)                              # proj.zip
        c.keys(ALT_F9, wait=0.4)
        c.keys(ENTER, wait=0.3)
        wait_until(lambda: "proj/a.txt" in c.right_files(), "single top-level folder")
        assert "proj/proj/a.txt" not in c.right_files() and "proj/sub/b.txt" in c.right_files(), sorted(c.right_files())
        c.keys(UP, UP, wait=0.12)                            # ".."
        c.keys(ALT_F9, wait=0.4)
        wait_for(c.sess, "Not a readable archive")           # not an archive: a clear message, no dialog
    finally:
        c.close()
    print("[PASS] AU3. Alt+F9: a subfolder for several top-level entries, none for a single folder, a message for a non-archive")


def test_archive_test():
    good = zip_bytes(TREE)
    bad = bytearray(zip_bytes([Expect("a.txt", data=b"A" * 3000), Expect("b.txt", data=b"B" * 3000)], zipfile.ZIP_STORED))
    bad[bad.index(b"AAAA") + 5] ^= 0xFF
    c = Case({"good.zip": good, "bad.zip": bytes(bad)})
    try:
        c.keys(DOWN, wait=0.15)                              # bad.zip (sorted first)
        c.keys(ALT_SHIFT_F9, wait=0.5)
        wait_for(c.sess, "1 member(s) could not be used")
        c.keys(DOWN, ALT_SHIFT_F9, wait=0.5)                 # good.zip
        wait_for(c.sess, "Archive is OK")
        assert c.right_files() == {}, "testing must not write anything"
    finally:
        c.close()
    print("[PASS] AU4. Test Archive: 'Archive is OK' for a good archive, the count of unusable members for a damaged one, nothing written")


def test_conflicts():
    c = Case({"a.zip": zip_bytes(TREE)})
    try:
        Path(c.right, "readme.txt").write_text("OLD")
        enter_archive(c)
        c.keys(F5, ENTER, wait=0.3)
        wait_for(c.sess, "File Conflict")
        c.keys(b"s", wait=0.3)                               # Skip
        wait_until(lambda: "bin.dat" in c.right_files(), "the rest arrives")
        wait_for(c.sess, "Unpacked")
        assert Path(c.right, "readme.txt").read_text() == "OLD"
        c.keys(F5, ENTER, wait=0.3)
        wait_for(c.sess, "File Conflict")
        c.keys(b"a", b"o", wait=0.3)                         # For all + Overwrite (the files of the first run exist now)
        wait_until(lambda: Path(c.right, "readme.txt").read_bytes() == b"hello archive\n", "overwritten")
    finally:
        c.close()
    print("[PASS] AU5. the File Conflict dialog: Skip keeps the file, Overwrite replaces it")


def test_evil():
    base = tempfile.mkdtemp(prefix="auv_")
    z = os.path.join(base, "evil.zip")
    with zipfile.ZipFile(z, "w") as zf:
        zf.writestr("fine.txt", b"fine")
        zf.writestr("../escape.txt", b"x")
        zf.writestr("a/../../escape2.txt", b"x")
    c = Case({"evil.zip": Path(z).read_bytes()})
    try:
        c.keys(DOWN, ALT_F9, wait=0.4)                       # the whole archive: the pane would hide the unsafe members
        c.keys(ENTER, wait=0.4)
        wait_for(c.sess, "2 member(s) could not be used")
        assert list(c.right_files()) == ["fine.txt"], c.right_files().keys()
        assert not os.path.exists(os.path.join(c.tmp, "escape.txt")) and not os.path.exists(os.path.join(c.tmp, "escape2.txt")), "a member escaped the destination"
    finally:
        c.close()
    print("[PASS] AU6. an evil archive: only the safe member arrives, the status counts the others, nothing escapes")


def test_cancel():
    big = zip_bytes([Expect(f"f{i}.bin", data=payload(50 + i, 24 * 1024 * 1024)) for i in range(8)], zipfile.ZIP_STORED)
    c = Case({"big.zip": big})
    try:
        c.keys(DOWN, ALT_F9, wait=0.3)
        wait_for(c.sess, "Unpack")
        c.keys(ENTER, wait=0.1)
        wait_for(c.sess, "Background", timeout=6)            # the progress dialog (the status bar shows "Unpacking" earlier)
        c.keys(b"c", wait=0.2)
        wait_for(c.sess, "Operation canceled", timeout=8)
        time.sleep(0.3)
        left = [p for p in Path(c.right).rglob("*") if "duop-part" in p.name]
        assert not left, f"temporary files left: {left}"
        done = [p for p in Path(c.right).rglob("f*.bin")]
        assert len(done) < 8, "the cancel came too late to prove anything"
        for p in done:
            assert p.stat().st_size == 24 * 1024 * 1024, f"{p.name} is partial under its real name"
    finally:
        c.close()
    print("[PASS] AU7. Cancel stops a big unpack: 'Operation canceled', no temporary file, no partial file under a real name")


def test_not_an_archive():
    c = Case({"fake.zip": os.urandom(5000)})
    try:
        c.keys(DOWN, ALT_F9, ENTER, wait=0.4)
        wait_for(c.sess, "Not a readable archive")
        assert c.right_files() == {}
        c.keys(ALT_SHIFT_F9, wait=0.4)
        wait_for(c.sess, "Not a readable archive")
    finally:
        c.close()
    print("[PASS] AU9. a random file named .zip: 'Not a readable archive' for unpack and test, nothing written")


def test_plain_gz_and_unsupported():
    c = Case({"a.dat": b"7z\xbc\xaf\x27\x1c" + bytes(300), "notes.txt.gz": gzip.compress(b"gzip text\n" * 30), "z.dat": zip_bytes(TREE)})
    try:
        c.keys(DOWN, wait=0.15)                              # a.dat: 7z by its magic
        c.keys(ALT_F9, wait=0.4)
        wait_for(c.sess, "cannot be browsed yet")            # a clear message, no dialog
        c.keys(DOWN, ALT_F9, wait=0.4)                       # notes.txt.gz
        wait_for(c.sess, "Unpack")
        c.keys(ENTER, wait=0.3)
        wait_until(lambda: c.right_files() == {"notes.txt": b"gzip text\n" * 30}, "the one file, no subfolder")
        c.keys(DOWN, ALT_F9, wait=0.4)                       # z.dat: a zip under another name
        c.keys(ENTER, wait=0.3)
        wait_until(lambda: "z.dat/readme.txt" in c.right_files(), "renamed zip unpacked (smart folder named after the file)")
    finally:
        c.close()
    print("[PASS] AU10. Alt+F9: a plain .gz unpacks to its file, a zip under another name unpacks, a 7z file says it is not supported yet")


def main():
    test_f5_whole_folder()
    test_selection()
    test_alt_f9_smart()
    test_archive_test()
    test_conflicts()
    test_evil()
    test_cancel()
    test_not_an_archive()
    test_plain_gz_and_unsupported()
    print("ALL ARCHIVE UNPACK UI TESTS PASSED.")


if __name__ == "__main__":
    main()
