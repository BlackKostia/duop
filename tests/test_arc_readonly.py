#!/usr/bin/env python3
"""Archive VFS is read-only until unpacking lands (docs/ARCHIVE_TOOLS_PLAN.md, phase 0).

F5 / F6 / F8 inside an archive pane used to open a transfer dialog whose task joined the *archive file* with the entry name
(arc_populate sets DirModel.path to the archive), so the copy could only fail late and confusingly.

ARO1 F5 on an entry of a .zip / .tar.gz opens the Unpack dialog (not Copy); Esc leaves nothing created in the other pane
ARO2 F6 and F8: a status message, no dialog (nothing moved, nothing deleted, the archive untouched)
ARO3 marked entries (Space) do not change that: F5 unpacks them, F8 is refused
ARO4 outside an archive F5 still opens the Copy dialog (guard is virtual_mode == 2 only)
"""
import io
import os
import sys
import tarfile
import tempfile
import time
import zipfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from test_pty import PtySession  # noqa: E402

BINARY = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "duop"))
DOWN, F5, F6, F8, ESC = b"\x1b[B", b"\x1b[15~", b"\x1b[17~", b"\x1b[19~", b"\x1b"
READONLY = "Archive is read-only"


def kill_session(sess):
    for step in (lambda: os.kill(sess.pid, 9), lambda: os.waitpid(sess.pid, 0), lambda: os.close(sess.master)):
        try:
            step()
        except OSError:
            pass


def wait_for(sess, text, timeout=3.0):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        sess.drain(0.08)
        if text in sess.vt.full_text():
            return
    raise AssertionError(f"timed out waiting for {text!r}:\n{sess.vt.full_text()}")


def make_dirs(tmp):
    left, right = os.path.join(tmp, "left"), os.path.join(tmp, "right")
    os.makedirs(left)
    os.makedirs(right)
    with zipfile.ZipFile(os.path.join(left, "a.zip"), "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("one.txt", b"one\n")
        z.writestr("two.txt", b"two\n")
    with tarfile.open(os.path.join(left, "b.tar.gz"), "w:gz") as t:
        for name in ("one.txt", "two.txt"):
            ti = tarfile.TarInfo(name)
            ti.size = 4
            t.addfile(ti, io.BytesIO(b"tar\n"))
    return left, right


def enter_archive(sess, downs):
    sess.drain(0.3)
    for _ in range(downs):
        sess.send(DOWN, wait=0.1)
    sess.send(b"\r", wait=0.3)
    wait_for(sess, "one.txt")
    sess.send(DOWN, wait=0.1)  # the cursor from ".." onto one.txt


def check_blocked(key, archive_downs, mark=False):
    with tempfile.TemporaryDirectory(prefix="aro_") as tmp:
        left, right = make_dirs(tmp)
        before = sorted(os.listdir(left))
        sess = PtySession(BINARY, ["--lang", "en", left, right], cols=120, rows=35)
        try:
            enter_archive(sess, archive_downs)
            if mark:
                sess.send(b" ", wait=0.15)
            sess.send(key, wait=0.4)
            wait_for(sess, READONLY)
            text = sess.vt.full_text()
            for title in (" Copy ", " Move ", "Confirm Delete"):
                assert title not in text, f"a dialog opened:\n{text}"
        finally:
            kill_session(sess)
        assert os.listdir(right) == [], f"something appeared in the other pane: {os.listdir(right)}"
        assert sorted(os.listdir(left)) == before, "the source directory changed"


def check_unpack_dialog(archive_downs, mark=False):
    with tempfile.TemporaryDirectory(prefix="aro_") as tmp:
        left, right = make_dirs(tmp)
        sess = PtySession(BINARY, ["--lang", "en", left, right], cols=120, rows=35)
        try:
            enter_archive(sess, archive_downs)
            if mark:
                sess.send(b" ", wait=0.15)
            sess.send(F5, wait=0.4)
            wait_for(sess, "Unpack")
            text = sess.vt.full_text()
            assert READONLY not in text and " Copy " not in text, text
            sess.send(ESC, wait=0.3)
        finally:
            kill_session(sess)
        assert os.listdir(right) == [], "nothing may be unpacked before the dialog is confirmed"


def test_aro1_copy_becomes_unpack():
    check_unpack_dialog(1)   # a.zip
    check_unpack_dialog(2)   # b.tar.gz
    print("[PASS] ARO1 F5 in an archive: the Unpack dialog, not Copy and not read-only")


def test_aro2_move_delete_blocked():
    check_blocked(F6, 1)
    check_blocked(F8, 1)
    check_blocked(F8, 2)
    print("[PASS] ARO2 F6 / F8 in an archive: read-only status, no dialog")


def test_aro3_marked_entries():
    check_unpack_dialog(1, mark=True)
    check_blocked(F8, 2, mark=True)
    print("[PASS] ARO3 marked entries: F5 unpacks them, F8 stays read-only")


def test_aro4_copy_outside_archive_unchanged():
    with tempfile.TemporaryDirectory(prefix="aro_") as tmp:
        left, right = make_dirs(tmp)
        sess = PtySession(BINARY, ["--lang", "en", left, right], cols=120, rows=35)
        try:
            sess.drain(0.3)
            sess.send(DOWN, wait=0.1)
            sess.send(F5, wait=0.4)
            wait_for(sess, "Copy")
            assert READONLY not in sess.vt.full_text()
            sess.send(ESC, wait=0.2)
        finally:
            kill_session(sess)
    print("[PASS] ARO4 F5 on a plain file still opens the Copy dialog")


if __name__ == "__main__":
    test_aro1_copy_becomes_unpack()
    test_aro2_move_delete_blocked()
    test_aro3_marked_entries()
    test_aro4_copy_outside_archive_unchanged()
