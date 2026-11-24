"""Mutation check of the xz / LZMA2 decoder (XZ8 of tests/test_c_xz_dec.py): each mutant changes one line of the sources, and some record of the
corpus - real streams, fuzz, hand-edited fields, small dictionaries - must give a different result."""

import random
import subprocess
from pathlib import Path

import c_harness as H
from test_c_inflate_stream import record, run as run_records
from xz_support import ROOT, SRCS, build, fuzz_cases, text, xz

MUT_FILES = ["src/fs/lzma_dec.c", "src/fs/lzma2_dec.c", "src/fs/xz_dec.c", "src/core/hash_crc64.c"]
# Not listed: checks whose removal changes no verdict because another check refuses the same streams - an LZMA2 end mark that is missing (the block
# check, padding and index then fail), the LZMA end marker (a distance of 2^32 - 1 lies beyond any history), LZMA properties above 224 (garbage decoding
# then ends in -EINVAL as well).
MUTANTS: list[tuple[str, str, str]] = [
    ("rc normalise", "if (z->range < (1u << 24)) {", "if (z->range < (1u << 23)) {"),
    ("prob up", "*p += (2048 - *p) >> 5;", "*p += (2048 - *p) >> 4;"),
    ("prob down", "*p -= *p >> 5;", "*p -= *p >> 4;"),
    ("direct bits", "v = (v << 1) + (mask + 1);", "v = (v << 1) + mask;"),
    ("len mid", "return MATCH_MIN + 8 + tree(z, l->mid[pos_state], 3);", "return MATCH_MIN + 7 + tree(z, l->mid[pos_state], 3);"),
    ("len high", "return MATCH_MIN + 16 + tree(z, l->high, 8);", "return MATCH_MIN + 15 + tree(z, l->high, 8);"),
    ("dist state", "len - MATCH_MIN < 4 ? len - MATCH_MIN : 3", "len - MATCH_MIN < 2 ? len - MATCH_MIN : 3"),
    ("dist model end", "if (slot < 14) {", "if (slot < 13) {"),
    ("dist align", "d = direct(z, limit - 4, d) << 4;", "d = direct(z, limit - 4, d) << 3;"),
    ("props lc lp", "return z->lc + z->lp <= 4;", "return z->lc + z->lp <= 5;"),
    ("literal probs", "init_probs(z->lit, (size_t)0x300 << (z->lc + z->lp));", "init_probs(z->lit, (size_t)0x300 << z->lc);"),
    ("rc first byte", "if (n < 5 || in[0] != 0) return -EINVAL;", "if (n < 5) return -EINVAL;"),
    ("matched literal", "off &= mbit;", "off &= ~mbit;"),
    ("literal state", "z->state = z->state < 4 ? 0 : z->state < 10 ? z->state - 3 : z->state - 6;", "z->state = z->state < 4 ? 0 : z->state < 10 ? z->state - 2 : z->state - 6;"),
    ("short rep state", "z->state = st < 7 ? 9 : 11;", "z->state = st < 7 ? 8 : 11;"),
    ("rep state", "z->state = st < 7 ? 8 : 11;\n    return decode_len", "z->state = st < 7 ? 9 : 11;\n    return decode_len"),
    ("rep3 shift", "                d = z->rep3;\n                z->rep3 = z->rep2;", "                d = z->rep3;"),
    ("match state", "z->state = z->state < 7 ? 7 : 10;", "z->state = z->state < 7 ? 7 : 11;"),
    ("distance check", "if (z->rep0 >= z->full || z->bad) return -EINVAL;", "if (z->bad) return -EINVAL;"),
    ("ring wrap", "if (src == z->size) src = 0;", ""),
    ("pending match", "z->pending = len - (uint32_t)n;", "z->pending = 0;"),
    ("pos state", "uint32_t pos_state = (uint32_t)z->pos & ((1u << z->pb) - 1);", "uint32_t pos_state = (uint32_t)z->pos & 1;"),
    ("dict reset by stored", "if (ctl >= 0xE0 || ctl == 1) {", "if (ctl >= 0xE0) {"),
    ("need dict reset", "} else if (*need_dict) {\n        return -EINVAL;", "} else if (0) {\n        return -EINVAL;"),
    ("stored control", "if (ctl > 2 || n - i < 2) return -EINVAL;", "if (n - i < 2) return -EINVAL;"),
    ("need props", "} else if (*need_props) {\n        return -EINVAL;", "} else if (0) {\n        return -EINVAL;"),
    ("state reset", "} else if (ctl >= 0xA0) {\n        lzma_reset_state(z);", "} else if (0) {\n        lzma_reset_state(z);"),
    ("chunk end", "return z->pending || !lzma_rc_finished(z) ? -EINVAL : 0;", "return 0;"),
    ("header crc", "if (load_le32(p + h->size - 4) != ~crc32_update(0xFFFFFFFFu, p, h->size - 4)) return -EINVAL;", ""),
    ("header reserved", "if (flags & 0x3C) return -EINVAL;", ""),
    ("header csize zero", "if (!a || !h->csize) return -EINVAL;", "if (!a) return -EINVAL;"),
    ("header sizes", "if ((h.has_c && h.csize != used) || (h.has_u && h.usize != d->out.block_out)) return -EINVAL;", ""),
    ("header padding", "if (p[pos]) return -EINVAL;                                 /* the header padding is zero */", ""),
    ("filter props", "if (k != nf - 1 || ps != 1 || p[pos] > 40) return -EINVAL;", "if (k != nf - 1 || p[pos] > 40) return -EINVAL;"),
    ("dictionary code", "(2u | (p[pos] & 1u)) << (p[pos] / 2 + 11)", "(2u | (p[pos] & 1u)) << (p[pos] / 2 + 10)"),
    ("dictionary limit", "return h->dict > XZ_DICT_MAX ? -EOPNOTSUPP : 0;", "return 0;"),
    ("block padding", "if (p[h.size + used + k]) return -EINVAL;", ""),
    ("check crc32", "if (c->kind == CHECK_CRC32) return load_le32(field) == ~c->crc32;", "if (c->kind == CHECK_CRC32) return true;"),
    ("check crc64", "if (c->kind == CHECK_CRC64) return load_le64(field) == ~c->crc64;", "if (c->kind == CHECK_CRC64) return true;"),
    ("check sha256", "return !__builtin_memcmp(hex, want, 65);", "return true;"),
    ("check sizes", "(size_t)4 << ((id - 1) / 3)", "(size_t)4 << ((id - 1) / 2)"),
    ("vli canonical", "if (i && !p[i]) return 0;", ""),
    ("index count", "if (!a || count != d->nrecs) return -EINVAL;", "if (!a) return -EINVAL;"),
    ("index records", "if (!a || up != d->recs[2 * k]) return -EINVAL;", "if (!a) return -EINVAL;"),
    ("index sizes", "if (!a || us != d->recs[2 * k + 1]) return -EINVAL;", "if (!a) return -EINVAL;"),
    ("index padding", "if (p[k]) return -EINVAL;\n    if (load_le32(p + padded)", "if (0) return -EINVAL;\n    if (load_le32(p + padded)"),
    ("index crc", "if (load_le32(p + padded) != ~crc32_update(0xFFFFFFFFu, p, padded)) return -EINVAL;", ""),
    ("footer backward", "|| ((uint64_t)load_le32(f + 4) + 1) * 4 != (uint64_t)ix) return -EINVAL;", ") return -EINVAL;"),
    ("footer flags", "if (f[8] != p[6] || f[9] != p[7]) return -EINVAL;", ""),
    ("footer magic", '__builtin_memcmp(p + pos + 10, "YZ", 2)', "0"),
    ("stream flags", "if (p[6] || (p[7] & 0xF0)) return -EINVAL;", ""),
    ("stream flags crc", "if (load_le32(p + 8) != ~crc32_update(0xFFFFFFFFu, p + 6, 2)) return -EINVAL;", ""),
    ("stream padding", "if (pad % 4) {", "if (0) {"),
    ("trailing padding", "if (pad == n) {", "if (0) {"),
    ("max_out", "bool over = k > room;", "bool over = false;"),
    ("crc64 poly", "#define POLY 0xC96C5795D7870F42ull", "#define POLY 0xC96C5795D7870F43ull"),
]


def mutation_records(smp: list[tuple[str, bytes, bytes]]) -> list[bytes]:
    from test_c_xz_dec import crafted_cases
    rnd = random.Random(21)
    recs = [record(s) for label, _, s in smp if len(s) < 40000][::2]
    recs += [record(c) for c in fuzz_cases(smp)[::2]]
    recs += [record(st) for _, st in crafted_cases()]
    recs += [record(xz(text(rnd, 300000), "crc32", dict_size=65536, preset=1)), record(xz(text(rnd, 400000), "crc32", dict_size=4096, preset=1)), record(xz(bytes(300000) + text(rnd, 100000), "none", dict_size=4096, preset=0)),
             record(xz(text(rnd, 90000) * 3, "sha256", preset=0, lc=0, lp=2, pb=0)), record(xz(rnd.randbytes(100000) + text(rnd, 50000), "crc64", preset=1))]
    _, p, s = next(x for x in smp if x[0].startswith("tiny crc64"))
    recs += [record(s + s, max_out=len(p) + 3), record(s, max_out=0), record(s, fail_after=100), record(s + bytes(4)), record(s + bytes(3))]
    return recs


def test_mutants(work: Path, smp: list[tuple[str, bytes, bytes]]) -> None:
    texts = {f: (ROOT / f).read_text() for f in MUT_FILES}
    for name, old, _ in MUTANTS:
        assert any(old in t for t in texts.values()), f"mutation anchor vanished: {name}"
    recs = mutation_records(smp)
    base = run_records(build(work / "mbase"), recs)
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
            for src in SRCS:
                local = md / Path(src).name
                srcs.append(str(local) if local.exists() and src in ("src/fs/" + local.name, "src/core/" + local.name) else src)
            try:
                same = run_records(build(md / "b", srcs=srcs), recs, timeout=120) == base
            except (subprocess.CalledProcessError, subprocess.TimeoutExpired, AssertionError):
                same = False
            if same:
                survivors.append(name)
        return go

    H.parallel([job(i, *m) for i, m in enumerate(MUTANTS)])
    assert not survivors, f"mutants not noticed: {sorted(survivors)}"
    print(f"[PASS] XZ8. {len(MUTANTS)}/{len(MUTANTS)} mutants of the decoder are noticed")
