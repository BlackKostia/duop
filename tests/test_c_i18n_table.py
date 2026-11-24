#!/usr/bin/env python3
"""The English table is data: src/tui/i18n_en.lang -> tools/gen_i18n.py -> build/i18n_en.c (lang_en_table + i18n_key_blob), and
src/tui/i18n_keys.c looks keys up in it. Diffed slot by slot / key by key against the frozen asm (tests/harness/i18n_table_reference.s,
i18n_keys_reference.s), with generator round-trips, failure modes, mutation checks and an aarch64 build whose dump must equal x86's."""

import random
import re
import subprocess
import sys
import tempfile
from pathlib import Path

import c_harness as H

ROOT = H.ROOT
sys.path.insert(0, str(ROOT / "tools"))
import gen_i18n as G  # noqa: E402

VERSION = (ROOT / "VERSION").read_text().strip()
TYPES = (ROOT / "include/types.inc").read_text()
LANG = ROOT / "src/tui/i18n_en.lang"
KEYS = G.slot_keys(TYPES)
CLI = ["tests/harness/i18n_table_cli.c", "src/tui/i18n_keys.c", "src/core/string.c", "src/core/string_path.c", "src/core/mem_prims.c",
       "src/core/syscall.c", "src/core/try_rt.c", "src/core/memory.c", "src/core/utf8.c"]


def generate(t: Path, lang: Path = LANG, gen: Path = ROOT / "tools/gen_i18n.py", name: str = "i18n_en.c") -> Path:
    out = t / name
    subprocess.run([sys.executable, str(gen), "--lang", str(lang), "--out", str(out)], check=True, capture_output=True)
    return out


def version_dir(t: Path) -> Path:
    ver = t / "ver"
    ver.mkdir(exist_ok=True)
    (ver / "version.inc").write_text(f'.macro DUOP_VER_ASCII\n    .ascii "{VERSION}"\n.endm\n.macro DUOP_VER_ASCIZ\n    .asciz "{VERSION}"\n.endm\n')
    (ver / "version.h").write_text(f'#pragma once\n#define DUOP_VERSION "{VERSION}"\n')
    return ver


def build(t: Path, gen_c: Path, arch: str = "x86_64") -> Path:
    ver = version_dir(t)
    objs: list[Path] = []
    if arch == "x86_64":
        for name, src in (("tab", "i18n_table_reference.s"), ("keys", "i18n_keys_reference.s")):
            o = t / f"{name}_ref.o"
            subprocess.run(["as", "-I", str(ROOT / "include/os/linux/x86_64"), "-I", str(ROOT / "include"), "-I", str(ver),
                            str(ROOT / "tests/harness" / src), "-o", str(o)], check=True)
            objs.append(o)
        H.rename_defined_globals(objs, "ref_")
    return H.build(arch, t, [*CLI, str(gen_c)], objs, cflags=["-I", str(ver)] + ([] if arch == "x86_64" else ["-DNO_REF"]))


def run(exe: Path, impl: str, *args: str, arch: str = "x86_64") -> str:
    res = H.run(arch, exe, [impl, *args])
    assert res.returncode == 0, (impl, args, res.stderr)
    return res.stdout.decode()


def both(exe: Path, *args: str) -> str:
    r, c = run(exe, "ref", *args), run(exe, "c", *args)
    assert r == c, f"asm and C differ for {args[:2]}:\n" + "\n".join(f"asm {a!r} | C {b!r}" for a, b in zip(r.split("\n"), c.split("\n")) if a != b)[:900]
    return c


def test_table(exe: Path) -> None:
    out = both(exe, "table")
    lines = out.splitlines()
    assert len(lines) == len(KEYS) == 287, len(lines)
    assert any(VERSION in ln for ln in lines), "{version} reaches the table"
    assert any("\\x0a" in ln for ln in lines) and any("\\xe2" in ln for ln in lines), "multi-line and UTF-8 slots are covered"
    print(f"[PASS] T1. all {len(lines)} slots of the generated English table are byte-identical to the asm table")


def test_keys(exe: Path) -> None:
    out = both(exe, "keys", *KEYS)
    got = dict(ln.rsplit("=", 1) for ln in out.splitlines())
    assert all(int(got[k]) == i * 8 for i, k in enumerate(KEYS)), "key #n names the slot at byte offset n*8"
    junk = ["", "code ", " code", "Code", "CODE", "cod", "codee", "f_labels", "f_labels.0", "f_labels.11", "f_labels.01", "help_lines.23",
            "help_lines.0", "help_instr_lines.37", "err_special.", "name\x01", "é", "x" * 300, "hdr_brand\t"]
    assert not set(junk) & set(KEYS)
    out = both(exe, "keys", *junk)
    assert all(ln.endswith("=-1") for ln in out.splitlines()), out
    print(f"[PASS] T2. i18n_key_offset: {len(KEYS)} keys and {len(junk)} near-miss keys identical to the asm lookup")


def test_roundtrip() -> None:
    rng = random.Random(77)
    alphabet = ["a", "Z", "9", " ", "\t", '"', "\\", "{", "}", "=", "#", "é", "日", "✓", G.VERSION_MARK, "n", "t", "{version}", "\\n", '\\"']
    for _ in range(3000):
        value = "".join(rng.choice(alphabet) for _ in range(rng.randrange(0, 9))).replace("{version}", G.VERSION_MARK)
        value = value.replace("\n", " ") if "\r" in value else value
        got = G.parse_lang(f"k = {G.format_value(value)}\n")["k"]
        assert got == value, (value, G.format_value(value), got)
    assert G.parse_lang('a="x"\nb = y \nc =\n# d = 1\n  e\t=\tz\n') == {"a": "x", "b": "y", "c": "", "e": "z"}
    assert G.parse_value('"  q  " junk') == "  q  " and G.parse_value("\\q\\\\") == "\\q\\" and G.parse_value("v{version}") == "v" + G.VERSION_MARK
    print("[PASS] T3. 3000 random values survive format -> parse; the generator's parser keeps the loader's quoting/escape rules")


def expect_failure(t: Path, text: str, what: str) -> None:
    lang = t / "bad.lang"
    lang.write_text(text, encoding="utf-8")
    out = t / "bad.c"
    res = subprocess.run([sys.executable, str(ROOT / "tools/gen_i18n.py"), "--lang", str(lang), "--out", str(out)], capture_output=True, text=True)
    assert res.returncode == 1 and "gen_i18n:" in res.stderr and not out.exists(), (what, res.returncode, res.stderr)


def test_failures() -> None:
    good = LANG.read_text(encoding="utf-8")
    with tempfile.TemporaryDirectory() as d:
        t = Path(d)
        expect_failure(t, good.replace("err_special         =", "err_special_x       ="), "renamed key = one slot missing + one unknown")
        expect_failure(t, "\n".join(ln for ln in good.splitlines() if not ln.startswith("cfg_truncated")), "missing slot")
        expect_failure(t, good + "code = XX\n", "duplicate key")
        expect_failure(t, good + "no.such.key = 1\n", "unknown key")
        (t / "types.inc").write_text(".struct 0\nFoo.x: .space 8\n")
        res = subprocess.run([sys.executable, str(ROOT / "tools/gen_i18n.py"), "--types", str(t / "types.inc"), "--lang", str(LANG), "--out", str(t / "o.c")],
                             capture_output=True, text=True)
        assert res.returncode == 1 and not (t / "o.c").exists()
    print("[PASS] T4. a missing, unknown or duplicate key and a types.inc without LangStrings fail the build and leave no output")


def test_mutations(t: Path) -> None:
    mutants = [("drop {version}", 'return " DUOP_VERSION ".join(pieces)', 'return "".join(pieces)'),
               ("no backslash escape", 'if ch == "\\\\" or ch == \'"\':', "if ch == '\"':"),
               ("array numbering from 0", 'f"{m.group(1)}.{i + 1}"', 'f"{m.group(1)}.{i}"'),
               ("array of one slot less", "for i in range(n)]", "for i in range(n - 1)]"),
               ("tab escape lost", 'ESCAPES = {"n": "\\n", "t": "\\t",', 'ESCAPES = {"n": "\\n",'),
               ("quote escape lost", '"\\\\", \'"\': \'"\'}', '"\\\\"}'),
               ("newline literal raw", 'elif ch == "\\n":\n                body.append("\\\\n")', 'elif ch == "\\n":\n                body.append(" ")'),
               ("version mark ignored", "out.append(VERSION_MARK)\n            i += 9", "out.append('{version}')\n            i += 9")]
    src = (ROOT / "tools/gen_i18n.py").read_text()
    good = (t / "i18n_en.c").read_text()
    killed = 0
    for i, (name, old, new) in enumerate(mutants):
        assert old in src, f"mutation anchor vanished: {name}"
        gen = t / "mut_gen.py"
        gen.write_text(src.replace(old, new, 1))
        out = t / f"mut{i}.c"
        res = subprocess.run([sys.executable, str(gen), "--lang", str(LANG), "--out", str(out)], capture_output=True)
        if res.returncode:                                              # the generator's own validation caught it
            killed += 1
            continue
        assert out.read_text() != good, f"mutant '{name}' left the generated C unchanged: the data never exercises it"
        (t / f"m{i}").mkdir()
        exe = build(t / f"m{i}", out)
        killed += run(exe, "ref", "table") != run(exe, "c", "table") or run(exe, "ref", "keys", *KEYS) != run(exe, "c", "keys", *KEYS)
    assert killed == len(mutants), f"{len(mutants) - killed} generator mutants survived"
    print(f"[PASS] T5. {killed}/{len(mutants)} generator mutations are caught (validation or the asm differential)")


def test_aarch64(t: Path, gen_c: Path, x86_exe: Path) -> None:
    if not H.available("aarch64"):
        print("[SKIP] T6: aarch64 toolchain unavailable")
        return
    x86 = run(x86_exe, "c", "table") + run(x86_exe, "c", "keys", *KEYS)
    a64 = t / "a64"
    a64.mkdir()
    exe = build(a64, gen_c, "aarch64")
    got = run(exe, "c", "table", arch="aarch64") + run(exe, "c", "keys", *KEYS, arch="aarch64")
    assert got == x86, "the generated English table and key lookup are the same on aarch64"
    print("[PASS] T6. the generated table links and runs under qemu-aarch64 with output identical to x86_64 (no asm table needed)")


def main() -> None:
    with H.workdir() as d:
        t = Path(d)
        gen_c = generate(t)
        assert generate(t, name="again.c").read_text() == gen_c.read_text(), "generation is deterministic"
        (t / "x").mkdir()
        exe = build(t / "x", gen_c)
        test_table(exe)
        test_keys(exe)
        test_roundtrip()
        test_failures()
        test_mutations(t)
        test_aarch64(t, gen_c, exe)
    print("ALL C I18N TABLE TESTS PASSED.")


if __name__ == "__main__":
    main()
