"""Mutation check of the bzip2 decoder (BZ8 of tests/test_c_bzip2_dec.py): each mutant changes one line of src/fs/bzip2_dec.c, and some record
of the corpus - real streams, truncations, fuzz, several streams, limits - must give a different result."""

import random
import subprocess
from pathlib import Path

import c_harness as H
from bz_support import ROOT, SRCS, build, bz, fuzz_cases, text
from test_c_inflate_stream import record, run as run_records

SRC = "src/fs/bzip2_dec.c"
# Not listed: checks whose removal changes no verdict that a stream can reach - a code longer than 20 bits (the tables of a decodable stream are
# complete), an empty symbol map or a single coding group (the rest of the block then fails to parse), a run of 2^21 symbols (the block size refuses it), level '0' (a block of size 0 holds nothing).
MUTANTS: list[tuple[str, str, str]] = [
    ("end of input", "else b->over = true;", ""),
    ("crc polynomial", "c << 1 ^ 0x04C11DB7u", "c << 1 ^ 0x04C11DB6u"),
    ("max_out", "bool over = n > room;", "bool over = false;"),
    ("code lengths range", "if (z->len[t][i] > mx) mx = z->len[t][i];", "if (z->len[t][i] > mx + 1) mx = z->len[t][i];"),
    ("limit", "vec += base[i + 1] - base[i];", "vec += base[i + 1] - base[i] + 1;"),
    ("base", "base[i] = ((limit[i - 1] + 1) << 1) - base[i];", "base[i] = (limit[i - 1] << 1) - base[i];"),
    ("code not in table", "return k < 0 || k >= MAX_ALPHA ? -1 : z->perm[t][k];", "return z->perm[t][k & 255];"),
    ("selector range", "if (++j >= groups) return -EINVAL;", ""),
    ("selector order", "        pos[0] = v;\n", ""),
    ("length range", "if (cur < 1 || cur > MAX_LEN) return -EINVAL;", "if (cur > MAX_LEN) return -EINVAL;"),
    ("length delta", "cur += get(b, 1) ? -1 : 1;", "cur += get(b, 1) ? 1 : -1;"),
    ("run size", "if (es > max_block - nblock) return -EINVAL;", ""),
    ("block size", "if (nblock >= max_block) return -EINVAL;", ""),
    ("selectors used up", "if (group >= nsel) return -EINVAL;\n                left = GROUP_SIZE;\n                group++;\n            }\n            left--;\n            sym = next_symbol(z, z->selector[group - 1]);\n            if (sym < 0 || b->over) return -EINVAL;\n        }",
     "if (group >= nsel) group = 1;\n                left = GROUP_SIZE;\n                group++;\n            }\n            left--;\n            sym = next_symbol(z, z->selector[group - 1]);\n            if (sym < 0 || b->over) return -EINVAL;\n        }"),
    ("run count byte", "if (run == 4) {", "if (run == 5) {"),
    ("run restart", "run = ch == prev ? run + 1 : 1;", "run = ch == prev ? run + 1 : 0;"),
    ("randomised", "if (get(b, 1)) return -EOPNOTSUPP;", "get(b, 1);"),
    ("origin", "if (origin >= (uint32_t)n) return -EINVAL;", ""),
    ("block crc", "if (~z->crc != stored) return -EBADMSG;", ""),
    ("combined crc", "*combined = (*combined << 1 | *combined >> 31) ^ stored;", "*combined ^= stored;"),
    ("stream crc", "r = b->over ? -EINVAL : stored != combined ? -EBADMSG : 0;", "r = b->over ? -EINVAL : 0;"),
    ("block magic", "if (m1 == 0x314159 && m2 == 0x265359) {", "if (m1 == 0x314159) {"),
    ("stream alignment", "z->b.n &= ~7u;", ""),
    ("move to front", "__builtin_memmove(z->yy + 1, z->yy, nn);", "__builtin_memmove(z->yy + 1, z->yy, nn ? nn - 1 : 0);"),
    ("cftab", "z->tt[z->cftab[uc]++] |= i << 8;", "z->tt[z->cftab[uc]++] |= (i + 1) << 8;"),
]


def mutation_records(smp: list[tuple[str, bytes, bytes]]) -> list[bytes]:
    rnd = random.Random(21)
    recs = [record(s) for _, _, s in smp if len(s) < 60000][::2]
    recs += [record(s[:c]) for _, _, s in smp if 30 < len(s) < 3000 for c in range(0, len(s), max(1, len(s) // 25))]
    recs += [record(c) for c in fuzz_cases(smp)]
    recs += [record(bz(text(rnd, 350000), 1)), record(bz(bytes(120000) + text(rnd, 80000), 1)), record(bz(rnd.randbytes(150000), 2))]
    small = [(p, s) for _, p, s in smp if 0 < len(p) < 3000][:4]
    recs += [record(small[0][1][:3] + b"0" + small[0][1][4:]), record(small[0][1][:3] + b"A" + small[0][1][4:]), record(small[0][1] + small[1][1]), record(small[0][1] + small[1][1] + small[2][1]), record(small[0][1] + b"junk")]
    p, s = small[0]
    recs += [record(s + s, max_out=len(p) + 3), record(s, max_out=0), record(s, fail_after=len(p) // 2 + 1)]
    return recs


def test_mutants(work: Path, smp: list[tuple[str, bytes, bytes]]) -> None:
    text_ = (ROOT / SRC).read_text()
    for name, old, _ in MUTANTS:
        assert old in text_, f"mutation anchor vanished: {name}"
    recs = mutation_records(smp)
    base = run_records(build(work / "mbase"), recs)
    survivors: list[str] = []

    def job(i: int, name: str, old: str, new: str):
        def go() -> None:
            md = work / f"m{i}"
            md.mkdir()
            p = md / "bzip2_dec.c"
            p.write_text(text_.replace(old, new, 1))
            try:
                same = run_records(build(md / "b", srcs=[str(p) if s == SRC else s for s in SRCS]), recs, timeout=120) == base
            except (subprocess.CalledProcessError, subprocess.TimeoutExpired, AssertionError):
                same = False
            if same:
                survivors.append(name)
        return go

    H.parallel([job(i, *m) for i, m in enumerate(MUTANTS)])
    assert not survivors, f"mutants not noticed: {sorted(survivors)}"
    print(f"[PASS] BZ8. {len(MUTANTS)}/{len(MUTANTS)} mutants of the decoder are noticed")
