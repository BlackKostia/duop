#!/usr/bin/env python3
"""src/fs/arc_index.c: the entry table of an archive (docs/ARCHIVE_TOOLS_PLAN.md, phase 1): dynamic growth, name normalisation, a
sorted de-duplicated order, exact lookup and the children of a folder in O(k log n) - checked against a plain Python model.

AI1 normalisation: "./x", "/x", "a//b", "a/./b" lose their noise, a trailing "/" or the DIR flag makes a directory, ".." marks the
    entry unsafe (kept, never listed), an empty result is skipped, a NUL or a name of PATH_MAX bytes is refused
AI2 300 random sessions (names built from awkward components: prefixes of each other, "b", "b-x", "b/", "b0", UTF-8, "..", duplicates):
    find and the folder listing equal the model for every folder and a sample of absent ones; the last duplicate wins
AI3 growth: 300000 entries added with no failure, 50 root folders, 6000 children in one, counters exact
AI4 aarch64 (qemu-user) gives the x86_64 output;  AI5 mutation check
"""

import random
import subprocess
import sys
from pathlib import Path

import c_harness as H

ROOT = H.ROOT
SRC = "src/fs/arc_index.c"
SRCS = ["tests/harness/arcidx_cli.c", SRC, "src/core/string.c", "src/core/utf8.c", "src/core/memory.c", "src/core/syscall.c",
        "src/core/try_rt.c", "src/core/mem_prims.c"]
DIR, SYM, UNSAFE = 1, 2, 16
PATH_MAX = 4096


def build(work: Path, arch: str = "x86_64", srcs: list[str] | None = None) -> Path:
    work.mkdir(parents=True, exist_ok=True)
    return H.build(arch, work, srcs or SRCS, cflags=["-I", str(ROOT / "tests/harness")])


def run(exe: Path, script: str, arch: str = "x86_64", timeout: int = 120) -> list[str]:
    res = H.run(arch, exe, [], script.encode(), timeout=timeout)
    assert res.returncode == 0, res.stderr[-300:]
    return res.stdout.decode().splitlines()


def normalize(raw: bytes, flags: int) -> tuple[bytes | None, bool, int | str]:
    """(name, unsafe, error): the rules of arc_idx_add."""
    if b"\0" in raw:
        return None, False, -22
    comps = [c for c in raw.split(b"/") if c not in (b"", b".")]
    if not comps:
        return None, False, 0
    isdir = raw.endswith(b"/") or bool(flags & DIR)
    name = b"/".join(comps) + (b"/" if isdir else b"")
    if len(name) >= PATH_MAX:
        return None, False, -36
    return name, b".." in comps, 0


class Model:
    def __init__(self) -> None:
        self.items: list[tuple[bytes, bool, int, int, int, bytes | None]] = []   # name, unsafe, flags, size, off, link
        self.unsafe = 0

    def add(self, raw: bytes, link: bytes | None, flags: int, size: int, off: int) -> int:
        name, unsafe, err = normalize(raw, flags)
        if name is None:
            return int(err)
        self.unsafe += unsafe
        self.items.append((name, unsafe, flags | (DIR if name.endswith(b"/") else 0) | (UNSAFE if unsafe else 0), size, off, link))
        return 0

    def finish(self) -> dict[bytes, tuple]:
        last: dict[bytes, tuple] = {}
        for it in self.items:
            last[it[0]] = it
        return last

    def children(self, uniq: dict[bytes, tuple], prefix: bytes) -> list[tuple[bytes, bool, int]]:
        names = sorted(n for n, it in uniq.items() if not it[1])
        out, i = [], 0
        while i < len(names):
            n = names[i]
            i += 1
            if not n.startswith(prefix) or len(n) == len(prefix):
                continue
            rest = n[len(prefix):]
            if b"/" not in rest:
                out.append((rest, False, uniq[n][3]))
                continue
            comp = rest.split(b"/")[0]
            sub = prefix + comp + b"/"
            out.append((comp, True, uniq[sub][3] if sub in uniq else -1))
            while i < len(names) and names[i].startswith(sub):
                i += 1
        return out


def hx(b: bytes) -> str:
    return b.hex() or "-"


def fmt_list(ch: list[tuple[bytes, bool, int]]) -> str:
    return "L" + "".join(f" {c.hex()}:{'d' if d else 'f'}:{s}" for c, d, s in ch) + " r=0"


COMPS = [b"a", b"b", b"b-x", b"b0", b"c", "é".encode(), b"..", b".", b"", b"x y", b"\x01", b"a.b"]


def random_name(rnd: random.Random) -> bytes:
    n = b"/".join(rnd.choice(COMPS) for _ in range(rnd.randrange(1, 5)))
    if rnd.random() < 0.3:
        n += b"/"
    if rnd.random() < 0.1:
        n = b"/" + n
    if rnd.random() < 0.1:
        n = b"./" + n
    return n


NORM_CASES = [(b"./x", 0), (b"/x", 0), (b"a//b", 0), (b"a/./b", 0), (b"a/b/", 0), (b"dir", DIR), (b"../x", 0), (b"a/../b", 0), (b"./", 0),
              (b"/", 0), (b".", 0), (b"", 0), (b"ok\0bad", 0), (b"n" * PATH_MAX, 0), (b"n" * (PATH_MAX - 1), 0), (b"d" * (PATH_MAX - 1), DIR),
              (b"d" * (PATH_MAX - 2), DIR)]
NORM_PROBES = (b"x", b"a/b", b"a/b/", b"dir/", b"../x", b"a/../b", b"ok")


def norm_script() -> str:
    s = "".join(f"A {raw.hex() or '-'} - {fl} 7 9\n" for raw, fl in NORM_CASES) + "F\n"
    return s + "".join(f"G {n.hex()}\n" for n in NORM_PROBES)


def test_normalisation(exe: Path) -> None:
    out = run(exe, norm_script())
    m = Model()
    want = [m.add(raw, None, fl, 7, 9) for raw, fl in NORM_CASES]
    k = len(NORM_CASES)
    assert [int(x.split()[1]) for x in out[:k]] == want, (out[:k], want)
    uniq = m.finish()
    assert out[k] == f"F 0 {len(uniq)}", out[k]
    for line, n in zip(out[k + 1:], NORM_PROBES):
        assert (line == "none") == (n not in uniq), (n, line)
    print(f"[PASS] AI1. normalisation: {k} awkward names behave like the model (noise stripped, '..' unsafe, empty skipped, NUL / PATH_MAX refused)")


def session(rnd: random.Random, n: int) -> tuple[str, list[str]]:
    m, script, want = Model(), "", []
    for i in range(n):
        raw = random_name(rnd)
        link = rnd.choice([None, None, b"target", b"../up", "é".encode()])
        flags = rnd.choice([0, 0, DIR, SYM])
        size = rnd.randrange(0, 10**6)
        script += f"A {raw.hex() or '-'} {link.hex() if link is not None else '-'} {flags} {size} {i}\n"
        want.append(f"A {m.add(raw, link, flags, size, i)}")
    uniq = m.finish()
    script += "F\n"
    want.append(f"F 0 {len(uniq)}")
    folders = {b""}
    for name in uniq:
        parts = name.rstrip(b"/").split(b"/")
        for k in range(1, len(parts) + 1):
            folders.add(b"/".join(parts[:k]) + b"/")
    folders |= {b"zz/", b"a/b", b"a/b/q/"}
    for f in sorted(folders)[:60]:
        script += f"L {f.hex() or '-'}\n"
        want.append(fmt_list(m.children(uniq, f)))
    for name in sorted(uniq)[:60] + [b"nothing", b"a/", b"b"]:
        script += f"G {name.hex()}\n"
        it = uniq.get(name)
        want.append("none" if it is None else f"{it[3]} {it[2]} {it[4]} {hx(it[5]) if it[5] is not None else '-'}")
    script += "S\n"
    want.append(f"{len(m.items)} {len(uniq)} {m.unsafe}")
    return script, want


def test_random(exe: Path) -> None:
    rnd = random.Random(0xA1D)
    for k in range(300):
        script, want = session(rnd, rnd.randrange(1, 120))
        got = run(exe, script)
        assert got == want, f"session {k}:\n" + "\n".join(f"  got  {g}\n  want {w}" for g, w in zip(got, want) if g != w)[:1500]
    print("[PASS] AI2. 300 random sessions: find / folder listing / counters equal the model (last duplicate wins, unsafe names never listed)")


def test_growth(exe: Path) -> None:
    n = 300000
    out = run(exe, f"X {n}\nF\nS\nL -\nL {b'd7/'.hex()}\nG {b'd7/f7'.hex()}\n", timeout=120)
    assert out[0] == "X 0" and out[1] == f"F 0 {n}" and out[2] == f"{n} {n} 0", out[:3]
    assert out[3].count(":d:") == 50, out[3][:200]
    assert out[4].count(":f:") == n // 50, out[4].count(":f:")
    assert out[5].startswith("7 "), out[5]
    print(f"[PASS] AI3. {n} entries: no failure, 50 root folders, {n // 50} children in one folder, lookup exact")


def test_aarch64(x86: Path, work: Path) -> None:
    if not H.available("aarch64"):
        print("[SKIP] AI4. aarch64 toolchain / qemu-user missing")
        return
    rnd = random.Random(0xA64)
    script = "".join(session(rnd, 80)[0] for _ in range(1))
    script += f"X 20000\nF\nL -\n"
    assert run(build(work / "a64", "aarch64"), script, "aarch64") == run(x86, script), "aarch64 and x86_64 disagree"
    print("[PASS] AI4. a session and 20000 growth entries give identical output under qemu-aarch64")


MUTANTS = [
    ("keep first duplicate", "if (i + 1 == n || name_cmp(x, a[i], a[i + 1]) != 0) a[u++] = a[i];",
     "if (i == 0 || name_cmp(x, a[i - 1], a[i]) != 0) a[u++] = a[i];"),
    ("unstable merge", "name_cmp(x, src[j], src[i]) < 0 ? src[j++] : src[i++]", "name_cmp(x, src[j], src[i]) <= 0 ? src[j++] : src[i++]"),
    ("sorted shortcut", "if (!is_sorted(x, a, n)) {", "if (false) {"),
    ("no subtree skip", "i = arc_idx_skip(x, i + 1, prefix, plen, rest, clen);", "i = i + 1;"),
    ("subtree key", "- '0';", "- '1';"),
    ("unsafe listed", "if (!rest[0] || (it->flags & ARC_F_UNSAFE)) {", "if (!rest[0]) {"),
    ("dir slash", "if (!is_dir) n--;", "if (false) n--;"),
    ("dot components", "if (len && !(len == 1 && raw[i] == '.')) {", "if (len) {"),
    ("dotdot unsafe", "if (len == 2 && raw[i] == '.' && raw[i + 1] == '.') *unsafe = true;", ""),
    ("empty skipped", "if (!n) return 0;", "if (false) return 0;"),
    ("NUL in name", "if (!name[i]) return -EINVAL;", ""),
    ("path max", "if (n >= PATH_MAX) return -ENAMETOOLONG;", "if (n > PATH_MAX) return -ENAMETOOLONG;"),
    ("lower bound", "x->items[x->order[mid]].name_off, path) < 0) lo = mid + 1;", "x->items[x->order[mid]].name_off, path) <= 0) lo = mid + 1;"),
    ("explicit folder entry", "r = fn(ctx, rest, clen, true, slash[1] ? nullptr : it);", "r = fn(ctx, rest, clen, true, nullptr);"),
    ("unsafe counter", "x->unsafe += unsafe;", ""),
]


def test_mutants(work: Path) -> None:
    text = (ROOT / SRC).read_text()
    for name, old, _ in MUTANTS:
        assert old in text, f"mutation anchor vanished: {name}"
    rnd = random.Random(0x3A7)
    script = "".join(session(rnd, 90)[0] for _ in range(1))
    scripts = [session(rnd, 90)[0] for _ in range(8)] + [script, norm_script()]
    base = [run(build(work / "mbase"), sc) for sc in scripts]
    survivors: list[str] = []

    def job(i: int, name: str, old: str, new: str):
        def go() -> None:
            d = work / f"m{i}"
            d.mkdir()
            p = d / "arc_index.c"
            p.write_text(text.replace(old, new, 1))
            try:
                exe = H.build("x86_64", d, [s if s != SRC else str(p) for s in SRCS], cflags=["-I", str(ROOT / "tests/harness")])
                same = all(run(exe, sc, timeout=30) == b for sc, b in zip(scripts, base))
            except (subprocess.CalledProcessError, subprocess.TimeoutExpired, AssertionError):
                same = False
            if same:
                survivors.append(name)
        return go

    H.parallel([job(i, *m) for i, m in enumerate(MUTANTS)])
    assert not survivors, f"mutants not noticed: {sorted(survivors)}"
    print(f"[PASS] AI5. {len(MUTANTS)}/{len(MUTANTS)} mutants of arc_index.c are noticed")


def main() -> None:
    with H.workdir() as d:
        w = Path(d).resolve()
        exe = build(w / "x86")
        test_normalisation(exe)
        test_random(exe)
        test_growth(exe)
        test_aarch64(exe, w)
        test_mutants(w)
    print("ALL C ARC INDEX TESTS PASSED.")


if __name__ == "__main__":
    sys.exit(main())
