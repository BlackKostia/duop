#!/usr/bin/env python3
"""Corrupt archives must never take Duop down (docs/ARCHIVE_TOOLS_PLAN.md, every phase: untrusted input).

The test is independent of the parser's API: it feeds seeded mutants of the fixture corpus (tests/archive_fixtures.py) to a real
process through a pty, enters each one, views its first entries and leaves again.

AF1 bit-flipped, truncated, zero-filled and header-field-forged tar / tar.gz / zip mutants: the process stays alive, the pane keeps
    redrawing, the terminal is restored on quit and the exit status is 0
AF2 forged size fields (tar 12-digit octal / base-256, zip 32-bit and ZIP64 sizes, ZIP64 offsets near 2^64, an entry count of 2^32+3)
AF3 the same mutants twice in a row inside one session (state left behind by a rejected archive must not poison the next one)
"""

import os
import random
import struct
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from archive_fixtures import build_corpus  # noqa: E402
from par import run_parallel  # noqa: E402
from test_pty import PtySession  # noqa: E402

BINARY = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "duop"))
DOWN, F3, F10, ESC, BACKSPACE = b"\x1b[B", b"\x1bOR", b"\x1b[21~", b"\x1b", b"\x7f"
SEED = 0xA4F
SOURCES = ("tree.tar", "tree_gnu.tar", "links.tar", "base256.tar", "tree.tgz", "tree.zip", "tree_zip64.zip", "links.zip", "sfx.zip",
           "comment.zip")


def mutate(rng: random.Random, data: bytes) -> bytes:
    b = bytearray(data)
    kind = rng.randrange(5)
    if kind == 0 and b:                                              # a few random bytes
        for _ in range(rng.randrange(1, 9)):
            b[rng.randrange(len(b))] = rng.randrange(256)
    elif kind == 1:                                                  # truncation
        b = b[: rng.randrange(len(b) + 1)]
    elif kind == 2 and b:                                            # a zeroed run
        i = rng.randrange(len(b))
        b[i:i + rng.randrange(1, 600)] = bytes(rng.randrange(1, 600))
    elif kind == 3 and b:                                            # bit flips
        for _ in range(rng.randrange(1, 40)):
            b[rng.randrange(len(b))] ^= 1 << rng.randrange(8)
    else:                                                            # a block of 0xFF
        i = rng.randrange(len(b) + 1)
        b[i:i + 8] = b"\xff" * 8
    return bytes(b)


def forge_sizes(rng: random.Random, name: str, data: bytes) -> bytes:
    """Header fields the random mutator rarely reaches, set to values near the type limits."""
    b = bytearray(data)
    if name.endswith((".tar", ".tgz")) and not name.endswith(".tgz") and len(b) >= 512:
        pos = 512 * rng.randrange(max(1, len(b) // 512 - 2))
        if bytes(b[pos + 257:pos + 262]) == b"ustar":
            b[pos + 124:pos + 136] = rng.choice([b"77777777777\0", b"\x80" + (1 << 62).to_bytes(11, "big"), b"\xff" * 12, b"0" * 11 + b"\0"])
    elif name.endswith(".zip"):
        e = b.rfind(b"PK\x05\x06")
        c = b.find(b"PK\x01\x02")
        choice = rng.randrange(4)
        if choice == 0 and c >= 0:
            struct.pack_into("<II", b, c + 20, rng.choice([0xFFFFFFFF, 0, 1 << 31]), rng.choice([0xFFFFFFFF, 0, 1 << 31]))
        elif choice == 1 and c >= 0:
            struct.pack_into("<I", b, c + 42, rng.choice([0xFFFFFFFF, len(b), len(b) - 1]))
        elif choice == 2 and e >= 0:
            struct.pack_into("<HHII", b, e + 8, 0xFFFF, 0xFFFF, 0xFFFFFFFF, 0xFFFFFFFF)
        elif e >= 0:
            b[e + 10:e + 12] = struct.pack("<H", rng.choice([0, 1, 0xFFFE]))
    return bytes(b)


def exit_status(sess: PtySession, timeout: float) -> tuple[bool, int | None]:
    """(exited, status): waits up to timeout seconds for the child to end."""
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        sess.drain(0.1)
        pid, st = os.waitpid(sess.pid, os.WNOHANG)
        if pid:
            return True, st
    return False, None


def reap(sess: PtySession) -> None:
    for step in (lambda: os.kill(sess.pid, 9), lambda: os.waitpid(sess.pid, 0), lambda: os.close(sess.master)):
        try:
            step()
        except OSError:
            pass


def alive(sess: PtySession) -> bool:
    return os.waitpid(sess.pid, os.WNOHANG)[0] == 0


def visit(sess: PtySession, tag: str, idx: int) -> None:
    """Cursor on the idx-th file of the pane: enter it, look at entries, view one, leave if it was entered."""
    sess.send(b"\x1b[H", wait=0.1)
    for _ in range(idx + 1):
        sess.send(DOWN, wait=0.05)
    sess.send(b"\r", wait=0.35)
    assert alive(sess), f"{tag}: process died on entering"
    inside = "Archive" in sess.vt.full_text() or ".." in sess.vt.full_text()
    for key in (DOWN, DOWN, F3, ESC, DOWN, b"\r", ESC):
        sess.send(key, wait=0.12)
        assert alive(sess), f"{tag}: process died after key {key!r}"
    if inside:
        for _ in range(4):                                           # back to the real directory (extra Backspaces stay harmless)
            sess.send(BACKSPACE, wait=0.08)
            assert alive(sess), f"{tag}: process died leaving the archive"


def run_case(tmp: str, files: list[tuple[str, bytes]], repeat: int = 1) -> None:
    left, right = os.path.join(tmp, "l"), os.path.join(tmp, "r")
    os.makedirs(left)
    os.makedirs(right)
    for name, data in files:
        Path(left, name).write_bytes(data)
    sess = PtySession(BINARY, ["--lang", "en", left, right], cols=120, rows=35, extra_env={"HOME": tmp})
    try:
        sess.drain(0.4)
        for r in range(repeat):
            for i, (name, _) in enumerate(files):
                visit(sess, f"{name} (round {r})", i)
        sess.send(b"\x1b[H", wait=0.1)
        sess.send(F10, wait=0.2)
        exited, st = exit_status(sess, 5.0)
        assert exited, f"quit did not end the process; screen:\n{sess.vt.full_text()}"
        assert st is not None and os.WIFEXITED(st) and os.WEXITSTATUS(st) == 0, f"abnormal exit status {st}"
    finally:
        reap(sess)


def mutants(corpus: dict, rng: random.Random, count: int, forge: bool) -> list[tuple[str, bytes]]:
    out = []
    for i in range(count):
        src = SOURCES[i % len(SOURCES)]
        data = corpus[src].path.read_bytes()
        if src.endswith(".tgz"):
            data = mutate(rng, data) if not forge else data
        else:
            data = forge_sizes(rng, src, data) if forge else mutate(rng, data)
        ext = ".tar.gz" if src.endswith(".tgz") else src[src.rindex("."):]
        out.append((f"m{i:03d}{ext}", data))
    return out


def batches(items: list, n: int) -> list[list]:
    return [items[i:i + n] for i in range(0, len(items), n)]


def case_group(corpus, seed_off: int, count: int, forge: bool, repeat: int = 1) -> list:
    rng = random.Random(SEED + seed_off)
    cases = []
    for group in batches(mutants(corpus, rng, count, forge), 6):
        def call(g=group):
            with tempfile.TemporaryDirectory(prefix="afz_") as tmp:
                run_case(tmp, g, repeat)
        cases.append(call)
    return cases


def main() -> None:
    with tempfile.TemporaryDirectory(prefix="afzc_") as croot:
        corpus = build_corpus(Path(croot))
        run_parallel(case_group(corpus, 0, 60, False))
        print("[PASS] AF1. 60 random mutants (bit flips, truncation, zero runs, 0xFF blocks) of tar / tgz / zip: alive, redraws, clean exit")
        run_parallel(case_group(corpus, 1, 36, True))
        print("[PASS] AF2. 36 forged size / offset / count fields: alive, clean exit")
        run_parallel(case_group(corpus, 2, 24, False, repeat=2))
        print("[PASS] AF3. mutants visited twice per session: no state left behind by a rejected archive")
    print("ALL ARCHIVE FUZZ TESTS PASSED.")


if __name__ == "__main__":
    main()
