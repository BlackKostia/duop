#!/usr/bin/env python3
"""src/fs/archive_vfs.c + arc_source.c + arc_extract.c: an archive as a virtual folder of a pane, on the new engine (mmap input,
ArcIndex, streaming extraction; docs/ARCHIVE_TOOLS_PLAN.md, phase 1) - no limit on archive size, entry count or entry size.

AV1 every fixture archive (tar PAX / GNU / ustar / base-256, tar.gz, tgz, zip stored / deflate / ZIP64 / CP437 / SFX / comment, links, empty):
    each folder (walked by Enter and ..) lists exactly the model's children and every regular file extracts to its exact bytes (CRC-32)
AV2 bzip2 / lzma zip members list but do not extract (-EOPNOTSUPP); an encrypted member lists and is refused; overlapping or ".." members
    never appear
AV3 scale: a 12 MiB zip, a tar.gz inflating to 40 MiB, 120000 tar members, 70000 zip members, an 80 MiB deflated entry (all above the
    old 4 MiB / 1024 / 8 MiB / 64 MiB limits): entered, listed, extracted
AV4 damage: a truncated tar enters flagged (r=3); a zip entry with a flipped byte fails its CRC; gzip with a bad trailer, random bytes
    named .zip / .tar, an empty file: not entered (r=4); ".." leaves the archive on its row
AV7 other names: zip-based files (.docx .jar .apk .whl .epub .xlsx ...) enter as archives (r=1); .xz .bz2 .7z .rar .zst .tar.xz and a plain .gz are
    known formats that are not supported yet (r=6); a .tar.gz that unpacks past the limit is r=5; a plain file is r=0
AV5 aarch64 (qemu-user) gives the x86_64 output;  AV6 mutation check;  AV7 see above
"""

import gzip
import os
import io
import re
import random
import struct
import subprocess
import sys
import tarfile
import zipfile
import zlib
from pathlib import Path

import c_harness as H
from archive_fixtures import Expect, build_corpus, payload, tar_bytes, zip_bytes
from test_c_arc_index import normalize

ROOT = H.ROOT
VFS = ["src/fs/archive_vfs.c", "src/fs/arc_source.c", "src/fs/arc_extract.c"]
NEW = ["src/fs/arc_index.c", "src/fs/arc_tar.c", "src/fs/arc_zip.c", "src/fs/gzip_dec.c", "src/fs/inflate_stream.c", "src/fs/archive_parse.c", "src/core/hash.c"]
MODS = ["src/fs/dir_model.c", "src/fs/dir_model_filter.c", "src/fs/dir_model_load.c", "src/fs/dir_model_stream.c", "src/fs/dir_model_stat.c",
        "src/fs/dir_model_select.c", "src/fs/dir_sort.c", "src/fs/dir_sort_radix.c", "src/fs/dir_cache.c", "src/fs/dir_search.c",
        "src/fs/dir_search_match.c", "src/fs/inflate.c", "src/fs/file_ops_delete.c", "src/fs/file_ops.c", "src/fs/file_ops_copy.c",
        "src/fs/file_ops_read.c", "src/fs/file_ops_overwrite.c", "src/fs/task.c", "src/fs/task_conflict.c", "src/core/oop_rt.c",
        "src/core/string_path.c", "src/core/tmpfile.c", "build/i18n_en.c"]
CORE = ["src/core/string.c", "src/core/utf8.c", "src/core/memory.c", "src/core/syscall.c", "src/core/try_rt.c", "src/core/mem_prims.c"]
EOPNOTSUPP = -95


def build(work: Path, arch: str = "x86_64", vfs: list[str] | None = None, cflags: list[str] | None = None) -> Path:
    work.mkdir(parents=True, exist_ok=True)
    inc = ["-I", str(ROOT / f"include/os/linux/{arch}"), "-I", str(ROOT / "include")]
    uring = work / "pal_uring.o"
    subprocess.run([*H.ARCHS[arch]["as_"], *inc, str(ROOT / f"src/os/linux/{arch}/pal_uring.s"), "-o", str(uring)], check=True, capture_output=True)
    stub = work / "stubs.c"
    stub.write_text('#include "duop_task.h"\n#include "duop_tui.h"\nconst LangStrings *i18n_get(void) { return &lang_en_table; }\n'
                    'int g_task_parent_pid; TaskShared *g_task_shm;\n')
    srcs = ["tests/harness/arcvfs_cli.c", str(stub), *(vfs or VFS), *NEW, *MODS, *[c for c in CORE if c not in MODS]]
    return H.build(arch, work, list(dict.fromkeys(srcs)), [uring], cflags=["-I", str(ROOT / "tests/harness"), "-I", str(ROOT / "build"), *(cflags or [])])


def run(exe: Path, script: str, arch: str = "x86_64", timeout: int = 300) -> list[str]:
    res = H.run(arch, exe, [], script.encode(), timeout=timeout)
    assert res.returncode == 0, res.stderr[-300:]
    return res.stdout.decode(errors="backslashreplace").splitlines()


def unesc(s: str) -> bytes:
    return re.sub(r"\\x([0-9a-f]{2})", lambda m: chr(int(m.group(1), 16)), s).encode("latin-1")


def listing(line: str) -> tuple[dict[str, str], dict[bytes, tuple[str, int]]]:
    """(header fields, {name: ('d'|'f', size)}) of a harness line; '..' is not part of the children."""
    head, _, rest = line.partition(" n=")
    fields = dict(f.split("=", 1) for f in (" n=" + rest).split() if "=" in f and ":" not in f.split("=")[0])
    kids: dict[bytes, tuple[str, int]] = {}
    for tok in (rest.split(" sub=", 1)[1].split(" ")[1:] if " sub=" in rest else []):
        name, flags, size = tok.rsplit(":", 2)
        if int(flags) & 32:                                          # FE_IS_PARENT
            continue
        kids[unesc(name)] = ("d" if int(flags) & 1 else "f", int(size))
    return fields, kids


def model_children(entries: list[Expect], prefix: bytes, zip_links: bool = False) -> dict[bytes, tuple[str, int]]:
    uniq: dict[bytes, Expect] = {}
    for e in entries:
        n, unsafe, _ = normalize(e.name.encode(), 1 if e.kind == "dir" else 0)
        if n is not None and not unsafe:
            uniq[n] = e
    kids: dict[bytes, tuple[str, int]] = {}
    for n, e in uniq.items():
        if not n.startswith(prefix) or n == prefix:
            continue
        rest = n[len(prefix):]
        comp = rest.split(b"/")[0]
        if b"/" in rest:
            kids.setdefault(comp, ("d", 0))
        else:
            kids.setdefault(comp, ("f", len(e.data) if e.kind == "file" else len(e.link) if zip_links and e.kind == "symlink" else 0))   # a zip symlink holds its target
    return kids


def plan(entries: list[Expect], archive: str) -> tuple[str, list]:
    """The script that enters the archive and visits every folder, and what each of its lines must show."""
    script = f"E {archive}\n"
    exp: list = [("E", b"", 1)]
    files = {normalize(e.name.encode(), 0)[0]: e for e in entries if e.kind == "file"}

    def walk(prefix: bytes) -> None:
        nonlocal script
        kids = model_children(entries, prefix, archive.endswith(".zip"))
        for comp in sorted(kids):
            kind, _ = kids[comp]
            if kind == "d":
                script += f"E {comp.decode()}\n"
                exp.append(("E", prefix + comp + b"/", 1))
                walk(prefix + comp + b"/")
                script += "U\n"
                exp.append(("U", prefix, 0))
            else:
                e = files.get(prefix + comp)
                if e is not None:
                    script += f"X {comp.decode()}\n"
                    exp.append(("X", e, 0))
    walk(b"")
    return script, exp


def check(name: str, entries: list[Expect], out: list[str], exp: list, strict_x: bool = True) -> None:
    zl = name.endswith(".zip")
    assert len(out) == len(exp), (name, len(out), len(exp), out[:3])
    stack: list[bytes] = [b""]
    for line, (kind, what, r) in zip(out, exp):
        if kind == "X":
            m = re.match(r"X r=(-?\d+)(?: len=(\d+) crc=(\d+))?", line)
            assert m, (name, line)
            if strict_x:
                assert int(m.group(1)) == 0 and int(m.group(2)) == len(what.data) and int(m.group(3)) == zlib.crc32(what.data), (name, what.name, line)
            else:
                assert int(m.group(1)) != 0, (name, what.name, line)
            continue
        m = re.match(rf"{kind} r=(\d+)", line)
        assert m and int(m.group(1)) == r, (name, line[:120], kind, r)
        fields, kids = listing(line)
        prefix = what if kind == "E" else what
        assert kids == model_children(entries, prefix, zl), (name, prefix, kids, model_children(entries, prefix, zl))
    # sanity: the walk visited everything it should have


def test_fixtures(exe: Path, work: Path) -> None:
    fx = build_corpus(work / "corpus", many=2000)
    names = [n for n, f in fx.items() if f.fmt in ("tar", "tgz", "zip") and f.entries and n not in ("tree_bzip2.zip", "tree_lzma.zip", "overlap.zip", "encrypted.zip")]
    script, plans = f"N {work / 'corpus'}\n", []
    for n in names:
        s, exp = plan(fx[n].entries, n)
        script += s + "U\n"
        plans.append((n, fx[n].entries, exp, s.count("\n")))
    out = run(exe, script)
    pos = 1                                                           # line 0 is the initial N listing
    for n, entries, exp, lines in plans:
        check(n, entries, out[pos:pos + lines], exp)
        pos += lines
        assert out[pos].startswith("U r="), (n, out[pos][:100])        # ".." out of the archive: the cursor lands on the archive row
        pos += 1
    print(f"[PASS] AV1. {len(names)} fixture archives: every folder lists the model's children, every file extracts to its exact bytes")


def test_special(exe: Path, work: Path) -> None:
    d = work / "special"
    d.mkdir()
    fx = build_corpus(d / "c")
    enc = fx["encrypted.zip"].path
    unsafe = d / "unsafe.zip"
    with zipfile.ZipFile(unsafe, "w") as zf:
        zf.writestr("../evil.txt", b"evil")
        zf.writestr("sub/../../worse.txt", b"worse")
        zf.writestr("ok.txt", b"ok")
    script = (f"N {d / 'c'}\nE encrypted.zip\nX readme.txt\nU\nE tree_bzip2.zip\nL\nX readme.txt\nU\nE tree_lzma.zip\nX readme.txt\nU\n"
              f"E overlap.zip\nL\nU\nN {d}\nE unsafe.zip\nL\nX ok.txt\n")
    out = run(exe, script)
    assert out[1].startswith("E r=1") and "readme.txt:0" in out[1], out[1][:160]            # encrypted: listed ...
    assert re.match(r"X r=-\d+", out[2]), out[2]                                           # ... and refused (-EACCES)
    assert out[2].startswith("X r=-13"), out[2]
    assert out[4].startswith("E r=1") and "readme.txt:0" in out[5] and out[6].startswith(f"X r={EOPNOTSUPP}"), (out[5][:100], out[6])
    assert out[9].startswith(f"X r={EOPNOTSUPP}"), out[9]
    assert listing(out[12])[1] == {}, f"overlapping members must not be listed: {out[12]}"
    kids = listing(out[16])[1]
    assert kids == {b"ok.txt": ("f", 2)}, f"'..' members must stay hidden: {kids}"
    assert out[17] == f"X r=0 len=2 crc={zlib.crc32(b'ok')}", out[17]
    print("[PASS] AV2. bzip2 / lzma members list but are refused (-EOPNOTSUPP), encrypted refused, overlapping and '..' members never listed")


def make_big(work: Path) -> dict[str, tuple[Path, list[Expect]]]:
    out: dict[str, tuple[Path, list[Expect]]] = {}
    b = work / "big"
    b.mkdir()
    blob = payload(31, 12 * 1024 * 1024)
    (b / "plain12.zip").write_bytes(zip_bytes([Expect("blob.bin", data=blob), Expect("tail.txt", data=b"tail\n")], zipfile.ZIP_STORED))
    out["plain12.zip"] = (b / "plain12.zip", [Expect("blob.bin", data=blob), Expect("tail.txt", data=b"tail\n")])
    huge = payload(32, 40 * 1024 * 1024)
    ents = [Expect("huge.bin", data=huge), Expect("after.txt", data=b"after\n")]
    (b / "big.tar.gz").write_bytes(gzip.compress(tar_bytes(ents), 1, mtime=0))
    out["big.tar.gz"] = (b / "big.tar.gz", ents)
    # 120000 empty tar members in 100 folders: built straight from headers (tarfile would take a minute)
    tar = io.BytesIO()
    names = [f"d{i % 100:03d}/f{i:06d}" for i in range(120000)]
    for nm in names:
        h = bytearray(512)
        h[0:len(nm)] = nm.encode()
        h[100:108] = b"0000644\0"
        h[108:124] = b"0000000\0" * 2
        h[124:136] = b"00000000000\0"
        h[136:148] = b"14400000000\0"
        h[148:156] = b" " * 8
        h[156] = ord("0")
        h[257:265] = b"ustar\x0000"
        h[148:156] = b"%06o\0 " % sum(h)
        tar.write(h)
    tar.write(b"\0" * 1024)
    (b / "many120k.tar").write_bytes(tar.getvalue())
    out["many120k.tar"] = (b / "many120k.tar", [Expect(nm) for nm in names])
    zents = [Expect(f"d{i % 70:02d}/f{i:05d}.txt", data=b"z%d" % i) for i in range(70000)]
    (b / "many70k.zip").write_bytes(zip_bytes(zents, zipfile.ZIP_STORED))
    out["many70k.zip"] = (b / "many70k.zip", zents)
    big_defl = payload(33, 80 * 1024 * 1024)
    bents = [Expect("big80.bin", data=big_defl)]
    (b / "defl80.zip").write_bytes(zip_bytes(bents, zipfile.ZIP_DEFLATED))
    out["defl80.zip"] = (b / "defl80.zip", bents)
    return out


def test_scale(exe: Path, work: Path) -> None:
    big = make_big(work)
    d = work / "big"
    script = f"N {d}\n"
    script += "E plain12.zip\nX blob.bin\nX tail.txt\nU\n"
    script += "E big.tar.gz\nX huge.bin\nX after.txt\nU\n"
    script += "E many120k.tar\nE d042\nX f004242\nU\nU\n"
    script += "E many70k.zip\nE d07\nX f00007.txt\nU\nU\n"
    script += "E defl80.zip\nX big80.bin\nU\n"
    out = run(exe, script, timeout=600)
    blob = big["plain12.zip"][1][0].data
    assert out[1].startswith("E r=1") and out[2] == f"X r=0 len={len(blob)} crc={zlib.crc32(blob)}" and out[3].startswith("X r=0 len=5 "), out[1:4]
    huge = big["big.tar.gz"][1][0].data
    assert out[5].startswith("E r=1") and out[6] == f"X r=0 len={len(huge)} crc={zlib.crc32(huge)}", out[5:7]
    assert out[9].startswith("E r=1") and len(listing(out[9])[1]) == 100, out[9][:100]
    assert len(listing(out[10])[1]) == 1200 and out[11].startswith("X r=0 len=0 "), (out[10][:100], out[11])
    assert out[14].startswith("E r=1") and len(listing(out[14])[1]) == 70, out[14][:100]
    assert len(listing(out[15])[1]) == 1000 and out[16].startswith("X r=0 len=2 "), (out[15][:80], out[16])
    d80 = big["defl80.zip"][1][0].data
    assert out[19].startswith("E r=1") and out[20] == f"X r=0 len={len(d80)} crc={zlib.crc32(d80)}", out[19:21]
    print("[PASS] AV3. 12 MiB zip, 40 MiB tar.gz, 120000 tar and 70000 zip members, an 80 MiB deflated entry: entered, listed, extracted exactly")


def test_damage(exe: Path, work: Path) -> None:
    d = work / "dmg"
    d.mkdir()
    ents = [Expect("a.txt", data=b"A" * 3000), Expect("b.txt", data=payload(5, 5000)), Expect("c.txt", data=b"c")]
    tar = tar_bytes(ents)
    (d / "cut.tar").write_bytes(tar[:512 + 3072 + 300])
    z = bytearray(zip_bytes(ents, zipfile.ZIP_STORED))
    z[z.index(b"AAAA") + 5] ^= 0xFF
    (d / "badcrc.zip").write_bytes(bytes(z))
    g = bytearray(gzip.compress(tar, 6, mtime=0))
    g[-6] ^= 1
    (d / "badtrailer.tar.gz").write_bytes(bytes(g))
    (d / "trunc.tgz").write_bytes(gzip.compress(tar, 6, mtime=0)[:200])
    (d / "fake.zip").write_bytes(random.Random(7).randbytes(3000))
    (d / "fake.tar").write_bytes(random.Random(8).randbytes(3000))
    (d / "empty0.zip").write_bytes(b"")
    script = (f"N {d}\nE cut.tar\nL\nX a.txt\nX b.txt\nU\nE badcrc.zip\nX a.txt\nX b.txt\nX c.txt\nU\nE badtrailer.tar.gz\nE trunc.tgz\nE fake.zip\nE fake.tar\nE empty0.zip\n"
              f"E badcrc.zip\nU\nU\n")
    o = run(exe, script)
    assert o[1].startswith("E r=3 ") and listing(o[1])[1] == {b"a.txt": ("f", 3000)}, o[1][:200]       # the member before the torn header
    assert o[3].startswith("X r=0 len=3000 ") and o[4] == "X r=-22", o[3:5]
    assert o[6].startswith("E r=1 "), o[6][:80]
    assert o[7] == "X r=-74", f"a flipped byte must fail the CRC-32: {o[7]}"
    assert o[8].startswith("X r=0 len=5000 ") and o[9].startswith("X r=0 len=1 "), o[8:10]
    assert all(l.startswith("E r=4 ") and " vm=0 " in l for l in o[11:16]), o[11:16]          # recognised, unreadable
    assert o[16].startswith("E r=1 ") and o[17].startswith("U r=") and " vm=0 " in o[17] and o[18].startswith("U r=-1"), o[16:19]
    print("[PASS] AV4. truncated tar enters flagged (r=3); flipped zip byte fails its CRC; bad gzip / random .zip .tar / empty file are not entered; '..' leaves the archive")


def test_other_names(exe: Path, work: Path) -> None:
    d = work / "names"
    d.mkdir()
    z = zip_bytes([Expect("word/document.xml", data=b"<w/>"), Expect("[Content_Types].xml", data=b"<t/>")])
    for n in ("a.docx", "b.jar", "c.apk", "d.whl", "e.epub", "f.xlsx", "g.odt", "h.pptx", "i.cbz", "j.nupkg"):
        (d / n).write_bytes(z)
    for n in ("k.tar.xz", "l.tar.bz2", "m.7z", "n.rar", "o.zst", "p.tar.zst", "q.xz", "r.bz2", "s.txz", "t.tbz2", "u.lz4"):
        (d / n).write_bytes(b"opaque " * 100)
    (d / "plain.gz").write_bytes(gzip.compress(b"just text\n" * 50))
    (d / "notes.txt").write_bytes(b"not an archive\n")
    (d / "w.tar.gz").write_bytes(gzip.compress(tar_bytes([Expect("x.bin", data=bytes(3 * 1024 * 1024))]), 1, mtime=0))
    names = sorted(os.listdir(d))
    out = run(exe, f"N {d}\n" + "".join(f"E {n}\nU\n" for n in names))
    got = {n: int(out[1 + 2 * i].split()[1][2:]) for i, n in enumerate(names)}
    for n in ("a.docx", "b.jar", "c.apk", "d.whl", "e.epub", "f.xlsx", "g.odt", "h.pptx", "i.cbz", "j.nupkg", "w.tar.gz"):
        assert got[n] == 1, (n, got[n])
    for n in ("k.tar.xz", "l.tar.bz2", "m.7z", "n.rar", "o.zst", "p.tar.zst", "q.xz", "r.bz2", "s.txz", "t.tbz2", "u.lz4", "plain.gz"):
        assert got[n] == 6, (n, got[n])
    assert got["notes.txt"] == 0, got["notes.txt"]
    small = run(build(work / "small", cflags=["-DARC_OPEN_TGZ_MAX=1048576ull"]), f"N {d}\nE w.tar.gz\nE a.docx\n")
    assert small[1].startswith("E r=5 ") and small[2].startswith("E r=1 "), (small[1][:40], small[2][:40])
    print("[PASS] AV7. zip-based files enter as archives; known unsupported formats are r=6; a too large .tar.gz is r=5; a plain file is r=0")


def test_aarch64(x86: Path, work: Path) -> None:
    if not H.available("aarch64"):
        print("[SKIP] AV5. aarch64 toolchain / qemu-user missing")
        return
    d = work / "corpus"
    script = f"N {d}\n" + "".join(f"E {n}\nL\nX readme.txt\nU\n" for n in ("tree.tar", "tree.tgz", "tree.zip", "cp437.zip", "links.zip"))
    a, b = run(build(work / "a64", "aarch64"), script, "aarch64"), run(x86, script)
    assert a == b, "aarch64 and x86_64 disagree"
    print("[PASS] AV5. five archives browse and extract identically under qemu-aarch64")


MUTANTS = [
    ("crc check", "if (~t.crc != it->crc32) return -EBADMSG;", ""),
    ("size check", "if (t.n != it->size) return -EBADMSG;", ""),
    ("unsupported method", "} else {\n        return -EOPNOTSUPP;\n    }", "} else {\n        return -EINVAL;\n    }"),
    ("encrypted refused", "if (it->flags & ARC_F_ENCRYPTED) return -EACCES;", ""),
    ("damaged flag", "return v->info.damaged ? 3 : 1;", "return 1;"),
    ("file wins over folder", "if (arc_idx_find(&p->v->idx, p->key) && is_dir) return 0;", ""),
    ("folder up", "while (i && v->sub[i - 1] != '/') i--;", "i = 0;"),
]


def test_mutants(work: Path) -> None:
    srcs = {s: (ROOT / s).read_text() for s in VFS}
    for name, old, _ in MUTANTS:
        assert any(old in t for t in srcs.values()), f"mutation anchor vanished: {name}"
    d = work / "mm"
    d.mkdir()
    fx = build_corpus(d / "c")
    ents = [Expect("a.txt", data=b"A" * 3000), Expect("b.txt", data=b"b")]
    (d / "c" / "cut.tar").write_bytes(tar_bytes(ents)[:512 + 3072 + 300])
    z = bytearray(zip_bytes(ents, zipfile.ZIP_STORED))
    z[z.index(b"AAAA") + 5] ^= 0xFF
    (d / "c" / "badcrc.zip").write_bytes(bytes(z))
    with zipfile.ZipFile(d / "c" / "fileanddir.zip", "w") as zf:
        zf.writestr("x", b"file")
        zf.writestr("x/y", b"deep")
    lie = bytearray(zip_bytes([Expect("lie.txt", data=b"hello hello hello hello")], zipfile.ZIP_DEFLATED))     # right CRC, declared size 100
    struct.pack_into("<I", lie, lie.index(b"PK\x03\x04") + 22, 100)
    struct.pack_into("<I", lie, lie.index(b"PK\x01\x02") + 24, 100)
    (d / "c" / "sizelie.zip").write_bytes(bytes(lie))
    script = (f"N {d / 'c'}\nE tree.zip\nL\nE ünï\nX é.txt\nU\nE docs\nE guide\nU\nL\nU\nU\nE tree.tar\nE docs\nU\nU\nE tree_lzma.zip\nX readme.txt\nU\nE encrypted.zip\nX readme.txt\nU\n"
              f"E sizelie.zip\nX lie.txt\nU\nE cut.tar\nU\nE badcrc.zip\nX a.txt\nU\nE fileanddir.zip\nL\nU\nE tree.tgz\nL\nU\n")
    base = run(build(work / "mbase"), script)
    survivors: list[str] = []

    def job(i: int, name: str, old: str, new: str):
        def go() -> None:
            md = work / f"m{i}"
            md.mkdir()
            vfs, patched = [], False
            for s in VFS:
                text = srcs[s]
                if not patched and old in text:
                    text = text.replace(old, new, 1)
                    patched = True
                p = md / Path(s).name
                p.write_text(text)
                vfs.append(str(p))
            try:
                same = run(build(md, vfs=vfs), script, timeout=40) == base
            except (subprocess.CalledProcessError, subprocess.TimeoutExpired, AssertionError):
                same = False
            if same:
                survivors.append(name)
        return go

    H.parallel([job(i, *m) for i, m in enumerate(MUTANTS)])
    assert not survivors, f"mutants not noticed: {sorted(survivors)}"
    print(f"[PASS] AV6. {len(MUTANTS)}/{len(MUTANTS)} mutants of the VFS modules are noticed")


def main() -> None:
    with H.workdir() as d:
        w = Path(d).resolve()
        exe = build(w / "x86")
        test_fixtures(exe, w)
        test_special(exe, w)
        test_scale(exe, w)
        test_damage(exe, w)
        test_other_names(exe, w)
        test_aarch64(exe, w)
        test_mutants(w)
    print("ALL C ARC VFS TESTS PASSED.")


if __name__ == "__main__":
    sys.exit(main())
