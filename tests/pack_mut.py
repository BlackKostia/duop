"""Mutation check of the archive writers (AP7 of tests/test_c_arc_pack.py): each mutant changes one line of the sources, and the archives it writes
for a fixed tree (every format, normal and reproducible metadata, ZIP64 forced) must differ from the unmutated writer's, or its report must."""

import shutil
import subprocess
import tempfile
from pathlib import Path

import arcpack_support as S
import c_harness as H

FILES = ["src/fs/tar_write.c", "src/fs/zip_write.c", "src/fs/arc_pack.c", "src/fs/arc_pack_out.c"]
MUTANTS: list[tuple[str, str, str]] = [
    ("octal digits", "dst[i] = (char)('0' + (v & 7));", "dst[i] = (char)('0' + ((v + 1) & 7));"),
    ("pax length", "if (put_decimal(t, (int64_t)total) == digits) break;", "if (put_decimal(t, (int64_t)total) >= digits) break;"),
    ("name split", "if (name[i] == '/' && i <= 155 && n - i - 1 <= 100 && i > 0) {", "if (name[i] == '/' && i <= 155 && n - i - 1 <= 99 && i > 0) {"),
    ("checksum", "h[155] = ' ';", "h[155] = 0;"),
    ("size limit", "bool big_size = size > OCT_MAX(11)", "bool big_size = size > OCT_MAX(10)"),
    ("uid limit", "big_uid = e->uid > OCT_MAX(7)", "big_uid = e->uid > OCT_MAX(6)"),
    ("link length", "if (llen > 100) ok", "if (llen > 99) ok"),
    ("pax padding", "if (!r && used % 512) {", "if (!r && used % 256) {"),
    ("data cap", "if (n > t->left) n = (size_t)t->left;", ""),
    ("zero fill", "while (t->left) {", "while (t->left > 1) {"),
    ("block padding", "return t->size % 512 ? emit(t, zeros, 512 - t->size % 512) : 0;", "return t->size % 512 ? emit(t, zeros, 511 - t->size % 512) : 0;"),
    ("end blocks", "static const uint8_t zeros[1024];\n    return emit(t, zeros, sizeof zeros);", "static const uint8_t zeros[1024];\n    return emit(t, zeros, 512);"),
    ("dos year", "if (year > 2107) year = 2107;", "if (year > 2106) year = 2106;"),
    ("dos time", "*dt = (uint16_t)(rem / 3600 << 11 | rem % 3600 / 60 << 5 | rem % 60 / 2);", "*dt = (uint16_t)(rem / 3600 << 11 | rem % 3600 / 60 << 5 | rem % 60);"),
    ("zip64 entry", "c->z64 = expect >= ARC_PACK_ZIP64_AT;", "c->z64 = expect > ARC_PACK_ZIP64_AT;"),
    ("zip method", "c->method = z->level > 0 && !dir && expect > 0 ? 8 : 0;", "c->method = z->level > 0 && !dir ? 8 : 0;"),
    ("zip attr", "(link ? 0120000u : dir ? 040000u : 0100000u) | (e->mode & 07777)", "(link ? 0100000u : dir ? 040000u : 0100000u) | (e->mode & 07777)"),
    ("zip descriptor", "put64(d + 8, c->csize);\n        put64(d + 16, c->usize);", "put64(d + 8, c->usize);\n        put64(d + 16, c->csize);"),
    ("zip central sizes", "put64(zx + zn, c->usize);\n        put64(zx + zn + 8, c->csize);", "put64(zx + zn, c->csize);\n        put64(zx + zn + 8, c->usize);"),
    ("zip utf8 flag", "put16(h + 6, 0x0808);                                         /* data descriptor, UTF-8 names */", "put16(h + 6, 0x0008);                                         /* data descriptor, UTF-8 names */"),
    ("zip entry count", "bool z64 = n >= 0xFFFF || cd_off >= 0xFFFFFFFFull || cd_size >= 0xFFFFFFFFull;", "bool z64 = n >= 0xFFFF + 1 || cd_off >= 0xFFFFFFFFull || cd_size >= 0xFFFFFFFFull;"),
    ("walk order", "str_cmp(v[j - gaps[g]], t) > 0", "str_cmp(v[j - gaps[g]], t) < 0"),
    ("fixed owner", "e->uid = fixed ? 0 : st->uid;", "e->uid = st->uid;"),
    ("fixed mode", "(type == '5' || (st->mode & 0111) ? 0755 : 0644)", "(type == '5' || (st->mode & 0111) ? 0775 : 0644)"),
    ("link mode", "e->mode = type == '2' ? 0777 :", "e->mode = type == '2' ? 0755 :"),
    ("link data", "if (!r && p->zip) r = data(p, (const uint8_t *)p->link, (size_t)n);", ""),
    ("special files", "return skip(p, -ENXIO_);", "return 0;"),
    ("self exclusion", "if (st.dev == p->out.dev && st.ino == p->out.ino) return 0;", ""),
    ("file identity", "now.dev != st->dev || now.ino != st->ino", "false"),
    ("bytes in", "p->r->bytes_in += (uint64_t)n;", ""),
    ("level fallback", "if (opts.level > 9) opts.level = 6;", "if (opts.level > 9) opts.level = 1;"),
    ("gzip level hint", "(uint8_t)(level >= 9 ? 2 : level == 1 ? 4 : 0)", "(uint8_t)(level >= 9 ? 4 : level == 1 ? 2 : 0)"),
    ("gzip trailer", "put32(t + 4, (uint32_t)o->usize);", "put32(t + 4, (uint32_t)o->usize + 1);"),
    ("no replace", "RENAME_NOREPLACE);\n        if (m == -EINVAL)", "0);\n        if (m == -EINVAL)"),
]


def scenario(exe: Path, tmp: Path) -> list:
    """Archives and reports for the fixed tree: every format, with real and with reproducible metadata."""
    S.make_tree(tmp)
    lines = []
    outs = []
    for fmt, ext in ((S.TAR, "tar"), (S.TGZ, "tar.gz"), (S.ZIP, "zip")):
        for lv, flags, tag in ((6, 0, "a"), (0, S.FIXED, "b"), (9, S.FIXED, "c"), (1, 0, "d")):
            dst = tmp / f"{tag}.{ext}"
            lines.append(S.P(dst, fmt, lv, tmp, ["src"], flags))
            outs.append(dst)
    lines.append(S.P(tmp / "sub.tar", S.TAR, 0, tmp / "src", ["deep", "small.txt", "link_rel", "fifo", "missing"]))
    outs.append(tmp / "sub.tar")
    for size, uid, mt in ((8589934591, 2097151, 8589934591), (8589934592, 2097152, 8589934592), (9 << 30, 3000000, -1), (0, 0, 0)):     # the edges of ustar
        lines.append("\t".join(["H", str(size), str(uid), str(mt), "0", "420", "e" * 60 + "/" + "f" * 60 + "/" + "g" * 60, ""]))
    lines.append("\t".join(["H", "3", "1", "5", "0", "420", "a" * 50 + "/" + "b" * 100, ""]))
    lines.append("\t".join(["H", "0", "1", "5", "2", "511", "lnk", "t" * 101]))
    lines.append("\t".join(["H", "0", "1", "5", "2", "511", "lnk", "t" * 100]))
    res = S.run(exe, lines)
    return [res, [p.read_bytes() if p.exists() else None for p in outs]]


def test_mutants(work: Path) -> None:
    texts = {f: (S.ROOT / f).read_text() for f in FILES}
    for name, old, _ in MUTANTS:
        assert any(old in t for t in texts.values()), f"mutation anchor vanished: {name}"
    z64 = ["-DARC_PACK_ZIP64_AT=1000ull"]
    base_dir = work / "mbase"
    base_dir.mkdir()
    base = scenario(S.build(base_dir / "b", cflags=z64), base_dir)
    survivors: list[str] = []

    def job(i: int, name: str, old: str, new: str):
        def go() -> None:
            md = work / f"m{i}"
            md.mkdir()
            patched, srcs = False, []
            for f, t in texts.items():
                if not patched and old in t:
                    t, patched = t.replace(old, new, 1), True
                (md / Path(f).name).write_text(t)
            for s in S.SRCS:
                srcs.append(str(md / Path(s).name) if s in FILES else s)
            try:
                work_dir = md / "w"
                work_dir.mkdir()
                same = scenario(S.build(md / "b", srcs=srcs, cflags=z64), work_dir) == base
            except (subprocess.CalledProcessError, subprocess.TimeoutExpired, AssertionError):
                same = False
            if same:
                survivors.append(name)
        return go

    H.parallel([job(i, *m) for i, m in enumerate(MUTANTS)])
    assert not survivors, f"mutants not noticed: {sorted(survivors)}"
    print(f"[PASS] AP7. {len(MUTANTS)}/{len(MUTANTS)} mutants of the archive writers change what they write")
