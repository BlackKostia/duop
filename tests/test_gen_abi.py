#!/usr/bin/env python3
"""tools/gen_abi.py (plan phase P1): C structs generated from include/classes.inc + types.inc.

G1 deterministic output; every asm struct is generated or already written by hand (nothing silently dropped)
G2 the generated header compiles with the hand-written headers (gcc x86_64, clang aarch64, project flags)
G3 every generated offset/size equals the assembler's, evaluated independently by `as` and by the C compiler
G4 synthetic .inc: `@c` type override, C keyword renaming, packed fallback for unaligned fields, parent embedding
G5 bad input fails loudly: unknown size symbol, unparsable FIELD line, first child field not at parent size
G6 `make build/duop_classes.h` produces the same file
"""

import json
import re
import shutil
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
GEN = ROOT / "tools/gen_abi.py"
PAL_INC = ROOT / "include/os/linux/x86_64"
CFLAGS = ["-std=gnu2x", "-ffreestanding", "-Wall", "-Wextra", "-Werror", "-funsigned-char", "-I", str(ROOT / "include")]
TARGETS = {"x86_64": ["gcc"], "aarch64": ["clang", "--target=aarch64-linux-gnu", "-Wno-unused-command-line-argument"]}


def run_gen(out: Path, *extra: str, check: bool = True) -> subprocess.CompletedProcess:
    cmd = ["python3", str(GEN), "--out", str(out), *extra]
    res = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
    if check:
        assert res.returncode == 0, f"gen_abi failed:\n{res.stdout}{res.stderr}"
    return res


def hand_written() -> set[str]:
    """Names with a full C body in include/duop_*.h (`struct N {` or `} N;`), independent of the generator."""
    names = set()
    for h in (ROOT / "include").glob("duop_*.h"):
        text = h.read_text(encoding="utf-8")
        names |= set(re.findall(r"\bstruct\s+(\w+)\s*\{", text)) | set(re.findall(r"\}\s*(\w+)\s*;", text))
    return names


def asm_struct_names() -> set[str]:
    names = set()
    for inc in ("classes.inc", "dialogs_ext.inc", "types.inc", "types_core.inc", "pal.inc"):
        lines = (ROOT / "include" / inc).read_text(encoding="utf-8").splitlines()
        names |= {m.group(1) for ln in lines if (m := re.match(r"CLASS_BEGIN\s+(\w+)", ln))}
        names |= {m.group(1) for ln in lines if (m := re.match(r"(\w+)\._size:", ln))}
    return names


def test_deterministic_and_complete(tmp: Path) -> dict:
    a, b, js = tmp / "a.h", tmp / "b.h", tmp / "l.json"
    run_gen(a, "--json", str(js))
    run_gen(b)
    assert a.read_bytes() == b.read_bytes(), "generator output is not deterministic"
    layout = json.loads(js.read_text())
    gen = {s["name"] for s in layout["structs"]}
    missing = asm_struct_names() - gen - hand_written()
    assert not missing, f"asm structs neither generated nor hand-written: {sorted(missing)}"
    assert not (gen & hand_written()), f"generated twice (hand-written wins): {sorted(gen & hand_written())}"
    for must in ("App", "FilePane", "Dialog", "HitZone", "Rect", "ConflictDialog", "DCSlot", "HeaderBar"):   # TaskShared / TaskSnap: duop_task.h
        assert must in gen, f"{must} must be generated"
    print(f"[PASS] G1. deterministic; {len(gen)} structs generated, {len(asm_struct_names() & hand_written())} stay hand-written")
    return layout


def compile_cmd(target: str, src: Path, obj: Path) -> list[str]:
    return [*TARGETS[target], *CFLAGS, "-O1", "-c", str(src), "-o", str(obj)]


def test_compiles(tmp: Path, header: Path) -> None:
    heads = sorted(p.name for p in (ROOT / "include").glob("duop_*.h"))
    src = tmp / "all.c"
    src.write_text("".join(f'#include "{h}"\n' for h in heads) + f'#include "{header}"\n' + "int dummy;\n")
    for target in TARGETS:
        if shutil.which(TARGETS[target][0]) is None:
            print(f"[SKIP] G2. {target}: {TARGETS[target][0]} not installed")
            continue
        res = subprocess.run(compile_cmd(target, src, tmp / f"all_{target}.o"), capture_output=True, text=True)
        assert res.returncode == 0, f"{target}: generated header does not compile:\n{res.stderr[:3000]}"
    print("[PASS] G2. generated header compiles together with every include/duop_*.h (gcc x86_64, clang aarch64)")


def nm_abs(obj: Path) -> dict[str, int]:
    out = {}
    for line in subprocess.run(["nm", str(obj)], capture_output=True, text=True, check=True).stdout.splitlines():
        m = re.match(r"([0-9a-fA-F]+)\s+[aAnN]\s+(\S+)$", line)
        if m:
            out[m.group(2)] = int(m.group(1), 16)
    return out


def asm_values(tmp: Path, names: list[str], inc: str = "classes.inc") -> dict[str, int]:
    src = tmp / "v.s"
    src.write_text('.intel_syntax noprefix\n.include "' + inc + '"\n.section .text\n' +
                   "".join(f".globl V_{i}\n.set V_{i}, {n}\n" for i, n in enumerate(names)))
    subprocess.run(["as", "-I", str(PAL_INC), "-I", str(ROOT / "include"), str(src), "-o", str(tmp / "v.o")],
                   check=True, capture_output=True)
    vals = nm_abs(tmp / "v.o")
    return {n: vals[f"V_{i}"] for i, n in enumerate(names)}


def c_values(tmp: Path, target: str, header: Path, exprs: list[str]) -> list[int]:
    body = "".join(f'    __asm__(".globl C_{i}\\n.set C_{i}, %c0" :: "i"((long long)({e})));\n' for i, e in enumerate(exprs))
    src = tmp / f"c_{target}.c"
    src.write_text('#include "duop_oop.h"\n#include "duop_fs.h"\n#include "duop_abi.h"\n' + f'#include "{header}"\nvoid probe(void)\n{{\n{body}}}\n')
    subprocess.run(compile_cmd(target, src, tmp / f"c_{target}.o"), check=True, capture_output=True)
    vals = nm_abs(tmp / f"c_{target}.o")
    return [vals[f"C_{i}"] for i in range(len(exprs))]


def test_offsets_match_assembler(tmp: Path, header: Path, layout: dict) -> None:
    pairs = []                                    # (asm symbol, C expression)
    for s in layout["structs"]:
        pairs.append((f"{s['name']}._size", f"sizeof({s['name']})"))
        pairs += [(f"{s['name']}.{f['asm']}", f"offsetof({s['name']}, {f['name']})") for f in s["fields"]]
    asm = asm_values(tmp, [a for a, _ in pairs])
    for target in TARGETS:
        if shutil.which(TARGETS[target][0]) is None:
            continue
        got = c_values(tmp, target, header, [e for _, e in pairs])
        for (a, e), v in zip(pairs, got):
            assert asm[a] == v, f"{target}: {e} = {v}, assembler {a} = {asm[a]}"
    assert len(pairs) > 250, f"suspiciously few layout points: {len(pairs)}"   # completeness itself is G1
    print(f"[PASS] G3. {len(pairs)} offsets/sizes: assembler == C on x86_64 and aarch64")


SYNTH = """.intel_syntax noprefix
.include "oop.inc"
CLASS_BEGIN Plain
    FIELD Plain.a,       4
    FIELD Plain.default, 4   # C keyword
    FIELD Plain.ptr,     8   # @c const char *
    FIELD Plain.buf,     2*4 # @c char[8]
    FIELD Plain.raw,     5   /* odd size -> byte array */
    FIELD Plain.tail,    3
CLASS_END Plain
CLASS_BEGIN Child
    EXTENDS Plain
    FIELD Child.x, 8
CLASS_END Child
.struct 0
Mis.small: .space 4
Mis.big:   .space 8
Mis._size:
"""


def synth_gen(tmp: Path, text: str, name: str = "t.inc", check: bool = True):
    inc = tmp / name
    inc.write_text(text)
    out = tmp / "t.h"
    res = run_gen(out, "--source", name, "-I", str(tmp), "-I", str(ROOT / "include"), "-I", str(PAL_INC),
                  "--handwritten", str(tmp / "none_*.h"), check=check)
    return res, out


def test_synthetic(tmp: Path) -> None:
    res, out = synth_gen(tmp, SYNTH)
    h = out.read_text()
    assert "const char * ptr;" in h, h
    assert "char buf[8];" in h and "uint8_t raw[5];" in h, h
    assert "default_;" in h and "default;" not in h.replace("default_;", ""), "C keyword field must be renamed"
    assert re.search(r"struct Child\s*\{\s*Plain base;", h), "child embeds its parent as `base`"
    head = lambda n: re.search(r"struct\s*(__attribute__\(\(packed\)\))?\s*" + n + r"\s*\{", h).group(0)
    assert "packed" in head("Mis"), "unaligned uint64_t at offset 4 needs a packed struct"
    assert "packed" not in head("Plain") and "packed" not in head("Child"), "naturally aligned structs must not be packed"
    src = tmp / "syn.c"
    src.write_text(f'#include "duop_abi.h"\n#include "{out}"\nint dummy;\n')
    subprocess.run(compile_cmd("x86_64", src, tmp / "syn.o"), check=True, capture_output=True)
    print("[PASS] G4. @c override, keyword renaming, parent embedding, packed fallback, header compiles")


def test_bad_input(tmp: Path) -> None:
    head = ".intel_syntax noprefix\n.include \"oop.inc\"\n"
    cases = {
        "unknown size symbol": (head + "CLASS_BEGIN Bad\n FIELD Bad.a, NO_SUCH_SYMBOL\nCLASS_END Bad\n", "NO_SUCH_SYMBOL"),
        "unparsable FIELD": (head + "CLASS_BEGIN Bad\n FIELD Bad.a 4\nCLASS_END Bad\n", "FIELD"),
        "child not at parent size": (head + "CLASS_BEGIN Par\n FIELD Par.a, 8\nCLASS_END Par\n"
                                     "CLASS_BEGIN Odd\n EXTENDS Par\n .struct 0\n FIELD Odd.b, 4\nCLASS_END Odd\n", "parent Par"),
        "unparsable struct line": (head + ".struct 0\nBad.a: .quad 1\nBad._size:\n", "Bad.a"),
    }
    for what, (text, needle) in cases.items():
        res, _ = synth_gen(tmp, text, check=False)
        assert res.returncode != 0 and needle in res.stderr + res.stdout, f"{what}: must fail mentioning {needle!r}\n{res.stderr}"
    print(f"[PASS] G5. {len(cases)} malformed inputs are rejected with a message")


def test_make_target(tmp: Path, header: Path) -> None:
    res = subprocess.run(["make", "-s", "-B", "build/duop_classes.h"], cwd=ROOT, capture_output=True, text=True)
    assert res.returncode == 0, res.stderr
    assert (ROOT / "build/duop_classes.h").read_bytes() == header.read_bytes(), "make target differs from the direct run"
    print("[PASS] G6. `make build/duop_classes.h` equals the direct generator output")


def main() -> None:
    with tempfile.TemporaryDirectory() as t:
        tmp = Path(t)
        header = tmp / "duop_classes.h"
        layout = test_deterministic_and_complete(tmp)
        run_gen(header)
        test_compiles(tmp, header)
        test_offsets_match_assembler(tmp, header, layout)
        test_synthetic(tmp)
        test_bad_input(tmp)
        test_make_target(tmp, header)
    print("ALL GEN_ABI TESTS PASSED.")


if __name__ == "__main__":
    main()
