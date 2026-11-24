"""Mutation check of the DEFLATE compressor (DF6 of tests/test_c_deflate.py): each mutant changes one line of the sources and must change the bytes
produced for some input of the corpus at some level (or break the stream, which the round trip then notices)."""

import subprocess
import zlib
from pathlib import Path

import c_harness as H
from test_c_deflate import ROOT, SRCS, build, deflate, samples

MUT_FILES = ["src/fs/deflate.c", "src/fs/deflate_huff.c"]
# Not listed: thresholds that change the output only at an exact tie or for data of a rare shape - the cost of a dynamic header by one bit, stored against
# dynamic within 36 bytes, fixed against dynamic at equal size, a header with 258 literal / length or 5 code-length codes, a distance code longer than 14 bits.
MUTANTS: list[tuple[str, str, str]] = [
    ("hash", "(uint32_t)p[0] << 10 ^ (uint32_t)p[1] << 5 ^ p[2]", "(uint32_t)p[0] << 10 ^ (uint32_t)p[1] << 4 ^ p[2]"),
    ("window limit", "uint32_t limit = d->strstart > DEF_MAX_DIST ? d->strstart - DEF_MAX_DIST : 0;", "uint32_t limit = d->strstart > DEF_MAX_DIST + 300 ? d->strstart - DEF_MAX_DIST - 300 : 0;"),
    ("good length", "if (prev_len >= d->good) chain >>= 2;", "if (prev_len >= d->good) chain >>= 1;"),
    ("nice length", "if (len >= nice) break;", "if (len > nice) break;"),
    ("chain limit", "&& --chain != 0);", "&& --chain > 1);"),
    ("max dist test", "d->strstart - head <= DEF_MAX_DIST) d->match_length = longest_match(d, head, DEF_MIN_MATCH - 1);", "d->strstart - head <= DEF_MAX_DIST + 200) d->match_length = longest_match(d, head, DEF_MIN_MATCH - 1);"),
    ("lazy limit", "d->prev_length < d->lazy && d->strstart - head <= DEF_MAX_DIST", "d->prev_length < d->lazy + 40 && d->strstart - head <= DEF_MAX_DIST"),
    ("too far", "d->strstart - d->match_start > 4096) d->match_length", "d->strstart - d->match_start > 2048) d->match_length"),
    ("lazy insert", "if (++d->strstart <= max_insert) (void)insert_string(d, d->strstart);", "if (++d->strstart <= max_insert && d->prev_length < 6) (void)insert_string(d, d->strstart);"),
    ("fast insert", "if (d->lookahead + d->match_length - k >= DEF_MIN_MATCH) (void)insert_string(d, d->strstart + k);", "if (k < 3 && d->lookahead + d->match_length - k >= DEF_MIN_MATCH) (void)insert_string(d, d->strstart + k);"),
    ("slide head", "d->head[i] = d->head[i] >= DEF_WSIZE ? (uint16_t)(d->head[i] - DEF_WSIZE) : 0;", "d->head[i] = d->head[i] >= DEF_WSIZE ? (uint16_t)(d->head[i] - DEF_WSIZE + 1) : 0;"),
    ("slide prev", "d->prev[i] = d->prev[i] >= DEF_WSIZE ? (uint16_t)(d->prev[i] - DEF_WSIZE) : 0;", "d->prev[i] = d->prev[i] >= DEF_WSIZE ? (uint16_t)(d->prev[i] - DEF_WSIZE) : d->prev[i];"),
    ("slide block start", "d->block_start -= DEF_WSIZE;", ""),
    ("symbols per block", "if (d->nsym == DEF_SYMS) {", "if (d->nsym == DEF_SYMS - 1) {"),
    ("stored block size", "if (d->lookahead == 65535) {", "if (d->lookahead == 65534) {"),
    ("pending literal", "tally_lit(d, d->win[d->strstart - 1]);\n            d->match_available = false;", "d->match_available = false;"),
    ("length code 258", "d->length_code[255] = 28;", ""),
    ("distance codes", "else d->dist_code[256 + (dm >> 7)] = (uint8_t)c;", "else d->dist_code[256 + (dm >> 7)] = (uint8_t)(c ? c - 1 : c);"),
    ("overflow repair", "bl_count[bits + 1] += 2;", "bl_count[bits + 1] += 1;"),
    ("lone symbol", "for (uint32_t s = 0; cnt < 2 && s < n; s++) {", "for (uint32_t s = 0; cnt < 1 && s < n; s++) {"),
    ("tie order", "(freq[sym[j - 1]] == freq[s] && sym[j - 1] > s)", "(freq[sym[j - 1]] == freq[s] && sym[j - 1] < s)"),
    ("repeat previous", "} else if (all[i] && run >= 4) {", "} else if (all[i] && run >= 5) {"),
    ("zero runs", "} else if (!all[i] && run >= 3) {", "} else if (!all[i] && run >= 4) {"),
    ("long zero runs", "used = run > 138 ? 138 : run;", "used = run > 100 ? 100 : run;"),
    ("fixed literal lengths", "for (uint32_t i = 0; i < 286; i++) fixed += (uint64_t)b->ll_freq[i] * (i < 144 ? 8 : i < 256 ? 9 : i < 280 ? 7 : 8);", "for (uint32_t i = 0; i < 286; i++) fixed += (uint64_t)b->ll_freq[i] * (i < 144 ? 8 : i < 256 ? 8 : i < 280 ? 7 : 8);"),
]


def fib_data() -> bytes:
    """Symbol counts 1, 1, 2, 3, 5, 8, ...: a Huffman tree deeper than 15 levels, which the length limit has to repair."""
    a, b, out = 1, 1, bytearray()
    for s in range(26):
        out += bytes([97 + s]) * a
        a, b = b, a + b
    import random
    p = list(out)
    random.Random(7).shuffle(p)
    return bytes(p)


def mutation_inputs(smp: dict[str, bytes]) -> list[tuple[str, bytes]]:
    far = smp["window"]
    return [("tiny", smp["tiny"]), ("text200k", smp["text200k"]), ("mixed", smp["mixed"]), ("elf", smp["elf"][:150000]), ("rep7", smp["rep7"][:150000]), ("edge", smp["edge"]),
            ("window", far), ("fib", fib_data()), ("zeros", smp["zeros1m"][:150000]), ("alpha16", smp["alpha16"][:100000]), ("per4", smp["per4"][:100000]), ("random100k", smp["random100k"]),
            ("longrun", smp["longrun"])]


def test_mutants(work: Path, smp: dict[str, bytes]) -> None:
    texts = {f: (ROOT / f).read_text() for f in MUT_FILES}
    for name, old, _ in MUTANTS:
        assert any(old in t for t in texts.values()), f"mutation anchor vanished: {name}"
    inputs = mutation_inputs(smp)
    cases = [(n, d, lv) for n, d in inputs for lv in (1, 4, 6, 9)] + [("text200k", smp["text200k"], 0)]
    base_exe = build(work / "mbase")

    def run_all(exe: Path) -> list[bytes]:
        return [deflate(exe, d, lv, 0, timeout=20) for _, d, lv in cases]          # serial: the mutants themselves run in parallel; a hang is a kill too

    base = run_all(base_exe)
    for (n, d, lv), o in zip(cases, base):
        assert zlib.decompress(o, -15) == d, f"the baseline does not round-trip: {n} level {lv}"
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
            for s in SRCS:
                srcs.append(str(md / Path(s).name) if s in MUT_FILES else s)
            try:
                same = run_all(build(md / "b", srcs=srcs)) == base
            except (subprocess.CalledProcessError, subprocess.TimeoutExpired, AssertionError):
                same = False                                    # a crash or an exit code is noticed too
            if same:
                survivors.append(name)
        return go

    H.parallel([job(i, *m) for i, m in enumerate(MUTANTS)])
    assert not survivors, f"mutants not noticed: {sorted(survivors)}"
    print(f"[PASS] DF6. {len(MUTANTS)}/{len(MUTANTS)} mutants of the compressor change its output")
