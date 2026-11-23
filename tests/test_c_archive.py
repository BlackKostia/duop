#!/usr/bin/env python3
"""src/fs/archive_parse.c versus the frozen assembly (tests/harness/archive_vfs_reference.s +
archive_extract_reference.s): the .tar / .tar.gz / .zip index and the archive as a virtual folder of a pane.

AR1 parsers, differential (x86_64): tar (files, directories, long names, sizes clamped to truncated data), zip (stored, deflated,
    directories, comments, ZIP64 records and extra fields), gzip (every header flag), and 600 random byte-flipped / truncated
    variants; the entries (name, offset, sizes, method, dir flag) or the gunzipped bytes
AR2 a defect of the asm, fixed: the ZIP central-directory bound "offset + 46 > length" wraps for a ZIP64 offset near 2^64, so a
    crafted archive made the parser read before its buffer (a crash when the buffer starts a mapping, as for the package manager, which parses without the fault
    guard); the C parser compares against the remaining length and returns no entries
AR3 (moved: the virtual folder is tests/test_c_arc_vfs.py)
AR4 qemu-aarch64: the C modules give the x86_64 results; AR5 mutation check
"""

import gzip
import io
import random
import struct
import subprocess
import tarfile
import zipfile
from pathlib import Path

import c_harness as H

ROOT = H.ROOT
SRCS = ["src/fs/archive_parse.c"]
REFS = ["tests/harness/archive_vfs_reference.s", "tests/harness/archive_extract_reference.s"]
MODS = ["src/fs/dir_model.c", "src/fs/dir_model_filter.c", "src/fs/dir_model_load.c", "src/fs/dir_model_stream.c", "src/fs/dir_model_stat.c",
        "src/fs/dir_model_select.c", "src/fs/dir_sort.c", "src/fs/dir_sort_radix.c", "src/fs/dir_cache.c", "src/fs/dir_search.c",
        "src/fs/dir_search_match.c", "src/fs/inflate.c", "src/fs/file_ops_delete.c", "src/fs/file_ops.c", "src/fs/file_ops_copy.c",
        "src/fs/file_ops_read.c", "src/fs/file_ops_overwrite.c", "src/fs/task.c", "src/fs/task_conflict.c", "src/core/oop_rt.c",
        "src/core/string_path.c", "src/core/tmpfile.c", "build/i18n_en.c"]      # the DirModel stack: the frozen asm object links it for its folder code
CORE = ["src/core/string.c", "src/core/utf8.c", "src/core/memory.c", "src/core/syscall.c", "src/core/try_rt.c", "src/core/mem_prims.c"]


def build(arch: str, t: Path, srcs: list[str] | None = None) -> Path:
    t.mkdir(parents=True, exist_ok=True)
    (s1,) = [s if Path(s).is_absolute() else str(ROOT / s) for s in (srcs or SRCS)]
    inc = ["-I", str(ROOT / f"include/os/linux/{arch}"), "-I", str(ROOT / "include")]
    uring = t / "pal_uring.o"
    subprocess.run([*H.ARCHS[arch]["as_"], *inc, str(ROOT / f"src/os/linux/{arch}/pal_uring.s"), "-o", str(uring)], check=True, capture_output=True)
    extra = [uring]
    if arch == "x86_64":
        refs = []
        for r in REFS:
            o = t / (Path(r).stem + ".o")
            subprocess.run(["as", *inc, str(ROOT / r), "-o", str(o)], check=True, capture_output=True)
            refs.append(o)
        H.rename_defined_globals(refs, "ref_")
        extra += refs
    stub = t / "stubs.c"
    stub.write_text('#include "duop_task.h"\n#include "duop_tui.h"\nconst LangStrings *i18n_get(void) { return &lang_en_table; }\n'
                    'int g_task_parent_pid; TaskShared *g_task_shm;\n')
    return H.build(arch, t, ["tests/harness/archive_cli.c", str(stub), *MODS, *CORE], extra,
                   cflags=["-I", str(ROOT / "tests/harness"), "-I", str(ROOT / "build"), f'-DAR_SRC1="{s1}"'])


def make_tar(rng: random.Random, gz: bool = False) -> bytes:
    bio = io.BytesIO()
    with tarfile.open(fileobj=bio, mode="w:gz" if gz else "w", format=tarfile.GNU_FORMAT) as tf:
        for i in range(rng.randrange(1, 12)):
            name = rng.choice(["a.txt", "dir/b.bin", "dir/sub/c", "x" * rng.randrange(90, 140), "ünï/é.txt", "deep/" * 5 + "f"]) + str(i)
            if rng.random() < 0.2:
                ti = tarfile.TarInfo(name + "_d")
                ti.type = tarfile.DIRTYPE
                tf.addfile(ti)
            data = bytes(rng.randrange(256) for _ in range(rng.choice([0, 1, 511, 512, 513, 3000])))
            ti = tarfile.TarInfo(name)
            ti.size = len(data)
            tf.addfile(ti, io.BytesIO(data))
    return bio.getvalue()


def make_zip(rng: random.Random, z64: bool = False) -> bytes:
    bio = io.BytesIO()
    with zipfile.ZipFile(bio, "w") as zf:
        for i in range(rng.randrange(1, 10)):
            name = rng.choice(["a.txt", "d/b.bin", "d/e/", "long" * 20, "ü/x"]) + ("" if i == 0 else str(i))
            method = rng.choice([zipfile.ZIP_STORED, zipfile.ZIP_DEFLATED])
            data = bytes(rng.randrange(4) for _ in range(rng.choice([0, 10, 5000])))
            if name.endswith("/"):
                zf.writestr(zipfile.ZipInfo(name), b"")
                continue
            zi = zipfile.ZipInfo(name)
            zi.compress_type = method
            with zf.open(zi, "w", force_zip64=z64) as w:
                w.write(data)
        if rng.random() < 0.3:
            zf.comment = b"c" * rng.randrange(0, 300)
    return bio.getvalue()


def crafted_zip64(cd_off: int) -> bytes:
    """An end-of-central-directory that defers to a ZIP64 record claiming the directory at cd_off."""
    z64 = struct.pack("<IQHHIIQQQQ", 0x06064B50, 44, 45, 45, 0, 0, 1, 1, 46, cd_off)
    loc = struct.pack("<IIQI", 0x07064B50, 0, 64, 1)
    eocd = struct.pack("<IHHHHIIH", 0x06054B50, 0, 0, 0xFFFF, 0xFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0)
    return b"PK\x03\x04" + b"\0" * 60 + z64 + loc + eocd


def to_zip64(data: bytes, total: int) -> bytes:
    """The same zip with its end record deferring to a ZIP64 record that claims `total` entries."""
    e = data.rindex(b"PK\x05\x06")
    cd_size, cd_off = struct.unpack_from("<II", data, e + 12)
    z = len(data[:e])
    rec = struct.pack("<IQHHIIQQQQ", 0x06064B50, 44, 45, 45, 0, 0, total, total, cd_size, cd_off)
    loc = struct.pack("<IIQI", 0x07064B50, 0, z, 1)
    eocd = struct.pack("<IHHHHIIH", 0x06054B50, 0, 0, 0xFFFF, 0xFFFF, cd_size, 0xFFFFFFFF, 0)
    return data[:e] + rec + loc + eocd


def crafted(d: Path) -> list[tuple[Path, str]]:
    """Inputs the random corpus rarely reaches: blank-padded tar sizes, 300-byte zip names, a stored entry whose central size exceeds
    its data, and a ZIP64 entry count above 2^32 (capped to ARC_MAX_ENTRIES before the 32-bit loop bound)."""
    out = []
    bio = io.BytesIO()
    with tarfile.open(fileobj=bio, mode="w") as tf:
        for n in ("a", "b"):
            ti = tarfile.TarInfo(n)
            ti.size = 700
            tf.addfile(ti, io.BytesIO(b"x" * 700))
    tb = bytearray(bio.getvalue())
    tb[124:136] = b"   1274 \0\0\0\0"                              # " 1274" octal = 700
    (d / "blank.tar").write_bytes(bytes(tb))
    out.append((d / "blank.tar", "t"))
    zb = io.BytesIO()
    with zipfile.ZipFile(zb, "w") as zf:
        for i in range(10):
            zf.writestr(("n" * 300 if i == 0 else f"f{i}"), b"stored-bytes" * (i + 1), compress_type=zipfile.ZIP_STORED)
    z = zb.getvalue()
    (d / "longname.zip").write_bytes(z)
    out.append((d / "longname.zip", "z"))
    zz = bytearray(z)
    c = zz.index(b"PK\x01\x02", zz.index(b"PK\x01\x02") + 1)    # the second central record: claim a larger size
    struct.pack_into("<I", zz, c + 24, 99999)
    (d / "bigsize.zip").write_bytes(bytes(zz))
    out.append((d / "bigsize.zip", "z"))
    (d / "z64count.zip").write_bytes(to_zip64(z, (1 << 32) + 3))
    out.append((d / "z64count.zip", "z"))
    return out


def mutate(rng: random.Random, data: bytes) -> bytes:
    b = bytearray(data)
    if rng.random() < 0.3:
        b = b[: rng.randrange(len(b) + 1)]
    for _ in range(rng.randrange(1, 8)):
        if b:
            b[rng.randrange(len(b))] = rng.randrange(256)
    return bytes(b)


def corpus(t: Path) -> list[tuple[Path, str]]:
    rng = random.Random(0xA4C)
    d = t / "corpus"
    d.mkdir(parents=True)
    out = []
    for i in range(30):
        base = {"t": make_tar(rng), "z": make_zip(rng, i % 3 == 0), "g": gzip.compress(make_tar(rng), mtime=0)}
        for kind, data in base.items():
            for j in range(7):
                p = d / f"{i}_{kind}_{j}"
                p.write_bytes(data if j == 0 else mutate(rng, data))
                out.append((p, kind))
    for k, flg in enumerate((0, 2, 4, 8, 16, 30)):                       # gzip header flags
        body = gzip.compress(b"hello gzip " * 100, mtime=0)
        hdr = bytearray(body[:10])
        hdr[3] = flg
        extra = (b"\x04\x00abcd" if flg & 4 else b"") + (b"name\0" if flg & 8 else b"") + (b"comment\0" if flg & 16 else b"") + (b"\0\0" if flg & 2 else b"")
        p = d / f"gzflag{k}"
        p.write_bytes(bytes(hdr) + extra + body[10:])
        out.append((p, "g"))
    return out + crafted(d)


def run(exe: Path, impl: str, script: str, arch: str = "x86_64") -> str:
    res = H.run_impl(arch, exe, impl, stdin=script.encode(), timeout=120)
    assert res.returncode == 0, (impl, res.returncode, res.stderr[-300:])
    return res.stdout.decode(errors="backslashreplace")


def parse_script(items: list[tuple[Path, str]]) -> str:
    return "".join(f"P {p} {k}\n" for p, k in items)


def test_parsers(exe: Path, t: Path) -> str:
    items = corpus(t)
    s = parse_script(items)
    a, b = run(exe, "ref", s).splitlines(), run(exe, "c", s).splitlines()
    diffs = [(x, y) for x, y in zip(a, b) if x != y]
    assert all(x == "P fault" for x, _ in diffs), f"oracle and C differ beyond asm faults: {diffs[:2]}"
    assert len(a) == len(b) == len(items)
    print(f"[PASS] AR1. tar / zip / gzip parsers identical to the asm on {len(items)} inputs "
          f"({sum('n=0' not in ln for ln in b)} with entries, {len(diffs)} where only the asm faulted)")
    return "\n".join(b)


def test_zip64_wrap(exe: Path, t: Path) -> None:
    p = t / "evil.zip"
    p.write_bytes(crafted_zip64((1 << 64) - 0x10))
    q = t / "fine.zip"
    q.write_bytes(crafted_zip64(64))
    s = f"P {p} Z\nP {q} Z\n"
    ref, c = run(exe, "ref", s).splitlines(), run(exe, "c", s).splitlines()
    assert ref[0] == "P fault" and c[0] == "P n=0", (ref, c)
    assert ref[1] == c[1], (ref[1], c[1])
    print("[PASS] AR2. fixed: a ZIP64 directory offset near 2^64 no longer wraps the bound check (asm: read outside the buffer)")


MUTANTS = [
    ("src/fs/archive_parse.c", "tar name cap", "(const char *)raw + off, 100);", "(const char *)raw + off, 101);"),
    ("src/fs/archive_parse.c", "tar size clamp", "if (size > avail) size = avail;", ""),
    ("src/fs/archive_parse.c", "tar dir flag", "e->is_dir = raw[off + 156] == '5';", "e->is_dir = 0;"),
    ("src/fs/archive_parse.c", "tar block round", "((size + 511) & ~(uint64_t)511)", "size"),
    ("src/fs/archive_parse.c", "octal blank", "if (p[i] == ' ') continue;", "if (p[i] == ' ') break;"),
    ("src/fs/archive_parse.c", "eocd window", "steps + 1 >= 65536", "steps + 1 >= 256"),
    ("src/fs/archive_parse.c", "zip64 entries cap", "if (*total > ARC_MAX_ENTRIES) *total = ARC_MAX_ENTRIES;", ""),
    ("src/fs/archive_parse.c", "zip64 extra ignored", "        if (xlen <= len - cd - 46 - nlen) arc_zip64_extra(e, r + 46 + nlen, xlen);\n", ""),
    ("src/fs/archive_parse.c", "zip name cap", "(nlen > 255 ? 255 : nlen) + 1", "(nlen > 254 ? 254 : nlen) + 1"),
    ("src/fs/archive_parse.c", "zip dir flag", "e->is_dir = n && e->name[n - 1] == '/';", "e->is_dir = 0;"),
    ("src/fs/archive_parse.c", "zip comp clamp", "if (e->comp_size > avail) e->comp_size = avail;", ""),
    ("src/fs/archive_parse.c", "zip stored clamp", "if (!e->method && e->size > e->comp_size) e->size = e->comp_size;", ""),
    ("src/fs/archive_parse.c", "zip comment skipped", "cd += 46 + nlen + xlen + clen;", "cd += 46 + nlen + xlen;"),
    ("src/fs/archive_parse.c", "gzip fname", "for (int f = 8; f <= 16; f += 8)", "for (int f = 16; f <= 16; f += 8)"),
    ("src/fs/archive_parse.c", "gzip hcrc", "if (flg & 2) p += 2;", ""),
    ("src/fs/archive_parse.c", "gzip cap floor", "if (cap < 65536) cap = 65536;", ""),
]


def test_mutants(t: Path, script: str, expect: str) -> None:
    src = (ROOT / SRCS[0]).read_text()
    for f, name, old, _ in MUTANTS:
        assert old in src, f"mutation anchor vanished: {name}"
    noticed: dict[str, bool] = {}

    def job(i: int, f: str, name: str, old: str, new: str):
        def go() -> None:
            mt = t / f"m{i}"
            mt.mkdir()
            p = mt / Path(f).name
            p.write_text(src.replace(old, new, 1))
            try:
                exe = build("x86_64", mt, [str(p)])
                noticed[name] = run(exe, "c", script).strip() != expect.strip()
            except (subprocess.CalledProcessError, subprocess.TimeoutExpired, AssertionError):
                noticed[name] = True
        return go

    H.parallel([job(i, *m) for i, m in enumerate(MUTANTS)])
    survivors = sorted(n for n, ok in noticed.items() if not ok)
    assert not survivors, f"mutants not noticed: {survivors}"
    print(f"[PASS] AR5. {len(MUTANTS)}/{len(MUTANTS)} mutants of the C parsers are noticed")


def main() -> None:
    with H.workdir() as d:
        t = Path(d).resolve()
        exe = build("x86_64", t / "x86")
        parsed = test_parsers(exe, t)
        test_zip64_wrap(exe, t)
        script = parse_script(corpus(t / "again"))
        if H.available("aarch64"):
            a64 = build("aarch64", t / "a64")
            assert run(a64, "c", script, "aarch64") == run(exe, "c", script), "qemu-aarch64 differs"
            print("[PASS] AR4. qemu-aarch64: the C parsers give the x86_64 results")
        else:
            print("[SKIP] AR4: aarch64 toolchain/qemu not installed")
        test_mutants(t, script, run(exe, "c", script))
    print("ALL C ARCHIVE TESTS PASSED.")


if __name__ == "__main__":
    main()
