"""Mutation check of the Zstandard decoder (ZS8 of tests/test_c_zstd_dec.py): each mutant changes one line of the sources, and some record of
the corpus - real streams, fuzz, hand-written blocks, patched headers - must give a different result in the product build or in the lax
one (ZSTD_LAX, see include/duop_zstd_int.h)."""

import random
import struct
import subprocess
from pathlib import Path

import c_harness as H
import zstd_craft as Z
from test_c_inflate_stream import record, run as run_records
from zstd_support import ROOT, SRCS, beyond_window, build, crafted_cases, fuzz_cases, text, zstd, frame_header, MAGIC

MUT_FILES = ["src/fs/zstd_dec.c", "src/fs/zstd_fse.c", "src/fs/zstd_huf.c", "src/fs/zstd_seq.c", "include/duop_zstd_int.h", "src/core/hash_xxh.c"]
# Not listed: checks whose removal changes no verdict because another check (or the strict end-of-stream test) refuses the same streams -
# "huf direct size", "huf last weight", "huf weights bound", "seq literals", "seq rle symbol", "fse sum / spread" (each one catches what the other lets by).
MUTANTS: list[tuple[str, str | tuple[str, ...], str | tuple[str, ...]]] = [
    ("fse spread step", "(size >> 1) + (size >> 3) + 3", "(size >> 1) + (size >> 3) + 2"),
    ("fse low cells", "t->cell[high--].sym = (uint8_t)s;", "t->cell[high].sym = (uint8_t)s;"),
    ("fse nbits", "uint32_t nb = log - zstd_hbit(ns);", "uint32_t nb = log - zstd_hbit(ns) + 1;"),
    ("fse threshold", "if (count >= threshold) count -= max;", ""),
    ("fse remaining", "remaining -= count < 0 ? -count : count;", "remaining -= count;"),
    ("fse zero runs", "n0 += 3;", "n0 += 2;"),
    ("fse ll default", "{4, 3, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 1, 1, 1,", "{4, 3, 2, 2, 2, 2, 2, 2, 2, 2, 2, 3, 2, 1, 1, 1,"),
    ("fse of default", "{1, 1, 1, 1, 1, 1, 2, 2, 2, 1,", "{1, 1, 1, 1, 1, 2, 1, 2, 2, 1,"),
    ("fse ml default", "{1, 4, 3, 2, 2, 2, 2, 2, 2, 1,", "{1, 3, 4, 2, 2, 2, 2, 2, 2, 1,"),
    ("huf nibbles", "(i & 1) ? src[1 + i / 2] & 15 : src[1 + i / 2] >> 4", "(i & 1) ? src[1 + i / 2] >> 4 : src[1 + i / 2] & 15"),
    ("huf ranks", "acc += count[k] << (k - 1);", "acc += count[k] << k;"),
    ("huf nbits", "(uint8_t)(mb + 1 - k)", "(uint8_t)(mb - k)"),
    ("huf segment", "seg = (dst_len + 3) / 4", "seg = dst_len / 4"),
    ("huf jump table", "if (s1 + s2 + s3 + 6 >= n) return -EINVAL;", ""),
    ("huf stream end", "return rev_finished(&b) || !ZSTD_STRICT ? 0 : -EINVAL;", "return 0;"),
    ("huf four streams", "if (seg * 3 > dst_len) return -EINVAL;", ""),
    ("seq first repeat", "uint32_t idx = (uint32_t)ofv - 1 + (ll == 0);", "uint32_t idx = (uint32_t)ofv - 1;"),
    ("seq third repeat", "offset = idx == 3 ? rep[0] - 1 : rep[idx];", "offset = idx == 3 ? rep[0] : rep[idx];"),
    ("seq repeat shift", "if (idx >= 2) rep[2] = rep[1];", ""),
    ("seq new offset", "        rep[2] = rep[1];\n        rep[1] = rep[0];\n        rep[0] = offset;\n        return offset;", "        rep[1] = rep[0];\n        rep[0] = offset;\n        return offset;"),
    ("seq window", "if (offset > c->pos || offset > c->window) return -EINVAL;", "if (offset > c->pos) return -EINVAL;"),
    ("seq history", "if (offset > c->pos || offset", "if (offset"),
    ("seq room", "(uint64_t)ll + ml > c->limit - c->pos", "0"),
    ("seq state order", "sml = cm->base + (uint32_t)rev_read(b, cm->nbits);\n            sof = co->base + (uint32_t)rev_read(b, co->nbits);",
     "sof = co->base + (uint32_t)rev_read(b, co->nbits);\n            sml = cm->base + (uint32_t)rev_read(b, cm->nbits);"),
    ("seq ll base", "48, 64, 128, 256", "48, 64, 129, 256"),
    ("seq ml base", "99, 131, 259, 515", "99, 131, 258, 515"),
    ("seq ml bits", "10, 11, 12, 13, 14, 15, 16};\nstatic const uint32_t ml_base", "10, 11, 12, 13, 14, 15, 15};\nstatic const uint32_t ml_base"),
    ("seq count", "+ 0x7F00;", "+ 0x7F01;"),
    ("seq repeat mode", "} else if (!(c->have & flag)) {", "} else if (0) {"),
    ("seq end", "return rev_finished(b) ? 0 : -EINVAL;", "return 0;"),
    ("seq reserved", "if (ZSTD_STRICT && (modes & 3)) return -EINVAL;", ""),
    ("seq rest", "size_t rest = c->lit_len - lpos;", "size_t rest = 0;"),
    ("seq no sequences", "} else if (pos != n) {", "} else if (0) {"),
    ("dec reserved bit", "if (d & 0x08) return -EINVAL;", ""),
    ("dec window limit", "if (f->window > (1ull << ZSTD_WINDOW_LOG_LIMIT)) return -EOPNOTSUPP;", ""),
    ("dec dictionary", "if (dict_id) return -EOPNOTSUPP;", ""),
    ("dec fcs 2 bytes", "if (fcsf == 1) fcs += 256;", ""),
    ("dec mantissa", "base + (base >> 3) * (wd & 7)", "base + (base >> 3) * (wd & 3)"),
    ("dec single segment", "f->window = single ? fcs : window;", "f->window = window;"),
    ("dec block type", "if (type == 3 || size > max_block", "if (size > max_block"),
    ("dec block size", "size > max_block || n - i < in", "n - i < in"),
    ("dec slide keep", "size_t keep = c->pos < c->window ? c->pos : (size_t)c->window;", "size_t keep = c->pos;"),
    ("dec checksum", "else if (load_le32(p + i) != (uint32_t)xxh64_digest(&d->xxh)) r = -EBADMSG;", ""),
    ("dec content size", "if (!r && f.has_fcs && c->total != f.fcs) r = -EINVAL;", ""),
    ("dec rle literals", "else __builtin_memset(c->lit, src[hdr], reg);", "else __builtin_memset(c->lit, 0, reg);"),
    ("dec literal size", "reg = hdr == 1 ? src[0] >> 3 :", "reg = hdr == 1 ? src[0] >> 2 :"),
    ("dec max_out", "bool over = k > room;", "bool over = false;"),
    ("dec skippable", "== 0x184D2A50u", "== 0x184D2A51u"),
    ("dec consumed", "res->consumed = rc ? res->consumed : pos;", "res->consumed = 0;"),
    ("dec repeat offsets", "c->rep[1] = 4;", "c->rep[1] = 5;"),
    ("rev small stream", "b->used = (8 - (uint32_t)n) * 8 + 8 - hb;", "b->used = (8 - (uint32_t)n) * 8 + 7 - hb;"),
    ("rev marker", "b->used = 8 - hb;", "b->used = 9 - hb;"),
    ("xxh prime", "#define P3 0x165667B19E3779F9ull", "#define P3 0x165667B19E3779F8ull"),
    ("xxh tail", "h = rotl(h ^ (uint64_t)load_le32(p) * P1, 23) * P2 + P3;", "h = rotl(h ^ (uint64_t)load_le32(p) * P1, 22) * P2 + P3;"),
]


def crafted(s: bytes, plain: bytes) -> list[bytes]:
    """Hand-made invalid streams and the records that probe the limits (as in ZS5), for the mutation run."""
    reserved, btype3 = bytearray(s), bytearray(s)
    reserved[4] |= 0x08
    btype3[6] |= 0x02
    toobig = MAGIC + bytes([0x00, 0x00]) + struct.pack("<I", 1 | (2000 << 3))[:3] + bytes(2000)
    dictid = frame_header(0x01, bytes([0x58, 0x07]) + bytes(20))
    wide = frame_header(0x00, bytes([18 << 3]) + bytes(20))
    badsum = bytearray(zstd(plain, "-3"))
    badsum[-1] ^= 1
    badfcs = bytearray(zstd(b"x" * 100, "--no-check", via_file=True))
    badfcs[5] = 101
    return [bytes(reserved), bytes(btype3), toobig, dictid, wide, bytes(badsum), bytes(badfcs), bytes.fromhex("28b52ffd2000010000"), s + b"junk", s[:-1]]


def literal_patches(s: bytes) -> list[bytes]:
    """Streams whose first block has Huffman literals, with the sizes in the literals header and the jump table rewritten."""
    if len(s) < 40 or s[4] & 0x20 or (s[9] & 3) != 2 or not (s[9] >> 2) & 3:
        return []
    fmt = (s[9] >> 2) & 3
    hdr, bits = {1: 3, 2: 4, 3: 5}[fmt], {1: 10, 2: 14, 3: 18}[fmt]
    v, mask = int.from_bytes(s[9:9 + hdr], "little"), (1 << bits) - 1
    reg, comp = (v >> 4) & mask, (v >> (4 + bits)) & mask

    def with_(r: int, c: int) -> bytes:
        nv = (v & ~((mask << 4) | (mask << (4 + bits)))) | (r << 4) | (c << (4 + bits))
        return s[:9] + nv.to_bytes(hdr, "little") + s[9 + hdr:]

    out = [with_(k, comp) for k in (1, 2, 3, 4, 5, 7)] + [with_(reg, c) for c in (3, 5, 8, 12) if c < comp] + [with_(reg, comp - d) for d in (1, 2, 5, 20) if comp > d]
    tl = 1 + s[9 + hdr] if s[9 + hdr] < 128 else 1 + (s[9 + hdr] - 127 + 1) // 2
    jt = 9 + hdr + tl
    for sizes in ((0xFFFF, 0, 0), (0, 0xFFFF, 0), (0x8000, 0x8000, 0x8000), (comp, comp, comp), (0, 0, 0)):
        patched = bytearray(s)
        patched[jt:jt + 6] = struct.pack("<3H", *sizes)
        out.append(bytes(patched))
    return out


def mutation_records(smp: list[tuple[str, bytes, bytes]]) -> list[bytes]:
    seen: set[str] = set()
    small = [(l, p, s) for l, p, s in smp if len(p) < 400000 and not (l.split()[0] + str(len(p)) in seen or seen.add(l.split()[0] + str(len(p))))]
    recs = [record(s) for _, _, s in smp if len(s) < 60000][::2]
    recs += [record(c) for c in fuzz_cases(smp)[::2]]
    label, plain, s = next(x for x in smp if x[0] == "tiny -3")
    recs += [record(c) for c in crafted(s, plain)]
    recs += [record(st) for _, st, _, _ in crafted_cases()[::5]] + [record(st) for _, st, _, _ in crafted_cases()[500:]]
    recs.append(record(beyond_window()))
    for lab in ("tiny -3", "alpha16 -3", "text200k -3", "mixed -3", "alpha16 -1"):
        recs += [record(c) for l, p, st in smp if l == lab for c in literal_patches(st)]
    q = Z.sequences_section([(3, 3, 6)])
    recs.append(record(Z.frame(Z.block(Z.literals_section("raw", b"abc") + q[:1] + bytes([q[1] | 1]) + q[2:]))))
    recs += [record(s + s, max_out=len(plain) + 3), record(s, max_out=0), record(s, fail_after=100)]
    rnd = random.Random(5)
    recs.append(record(zstd(text(rnd, 3 << 20) + bytes(100000) + text(rnd, 200000), "-3", "--zstd=wlog=10")))      # slides the history
    recs.append(record(zstd(rnd.randbytes(100000) + b"x" * 90000, "-1", via_file=True)))
    return recs + [record(s) for _, _, s in small[:30]]


def test_mutants(work: Path, smp: list[tuple[str, bytes, bytes]]) -> None:
    texts = {f: (ROOT / f).read_text() for f in MUT_FILES}
    for name, old, _ in MUTANTS:
        for o in old if isinstance(old, tuple) else (old,):
            assert any(o in t for t in texts.values()), f"mutation anchor vanished: {name}"
    recs = mutation_records(smp)
    base = [run_records(build(work / "mbase"), recs), run_records(build(work / "mbase_lax", lax=True), recs)]    # the product build, and the one that tolerates more
    survivors: list[str] = []

    def job(i: int, name: str, old: str | tuple[str, ...], new: str | tuple[str, ...]):
        def go() -> None:
            md = work / f"m{i}"
            md.mkdir()
            patched, srcs = False, []
            pairs = list(zip(old, new)) if isinstance(old, tuple) else [(old, new)]
            for f, t in texts.items():
                for o, n in pairs:
                    if o in t:
                        t, patched = t.replace(o, n, 1), True
                (md / Path(f).name).write_text(t)
            for src in SRCS:
                name_ = Path(src).name
                srcs.append(str(md / name_) if (md / name_).exists() and src.startswith(("src/fs/zstd_", "src/core/hash_xxh")) else src)
            try:
                same = all(run_records(build(md / f"b{k}", srcs=srcs, lax=bool(k)), recs, timeout=60) == base[k] for k in (0, 1))     # noticed in either build
            except (subprocess.CalledProcessError, subprocess.TimeoutExpired, AssertionError):
                same = False
            if same:
                survivors.append(name)
        return go

    H.parallel([job(i, *m) for i, m in enumerate(MUTANTS)])
    assert not survivors, f"mutants not noticed: {sorted(survivors)}"
    print(f"[PASS] ZS8. {len(MUTANTS)}/{len(MUTANTS)} mutants of the decoder are noticed")
