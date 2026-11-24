/* src/fs/zstd_fse.c - the finite state entropy tables of Zstandard (RFC 8878 section 4.1; freestanding C23).
 *
 * A table description is a little-endian bit sequence: the accuracy log, then one count per symbol (-1 = "less than one", 0 =
 * unused, with run-length coded zeros). From the counts the decoding table is spread over its cells and every cell receives the
 * number of bits to read and the base of the next state. The description is untrusted: the counts must add up to exactly the table
 * size, the spread must end where it started, and nothing is read beyond the bytes handed in. */
#include "duop_zstd_int.h"

/* the default distributions (RFC 8878 section 3.1.1.3.2.2) */
static const int8_t ll_norm[36] = {4, 3, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 3, 2, 1, 1, 1, 1, 1, -1, -1, -1, -1};
static const int8_t ml_norm[53] = {1, 4, 3, 2, 2, 2, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
                                   1, 1, 1, 1, 1, 1, 1, 1, -1, -1, -1, -1, -1, -1, -1};
static const int8_t of_norm[29] = {1, 1, 1, 1, 1, 1, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, -1, -1, -1, -1, -1};

/* the cells from the counts: -EINVAL when they cannot be a distribution */
static int fse_build(FseTable *t, const int16_t *norm, uint32_t nsym, uint32_t log)
{
    uint32_t size = 1u << log, mask = size - 1, high = size - 1;
    uint16_t next[256];
    uint32_t minus = 0;
    for (uint32_t s = 0; s < nsym; s++) minus += norm[s] < 0;
    if (minus > size) return -EINVAL;
    for (uint32_t s = 0; s < nsym; s++) {
        if (norm[s] < 0) {
            t->cell[high--].sym = (uint8_t)s;                   /* "less than one": the top cells, one state each */
            next[s] = 1;
        } else {
            next[s] = (uint16_t)norm[s];
        }
    }
    uint32_t step = (size >> 1) + (size >> 3) + 3, pos = 0;
    for (uint32_t s = 0; s < nsym; s++) {
        for (int32_t i = 0; i < norm[s]; i++) {
            t->cell[pos].sym = (uint8_t)s;
            do pos = (pos + step) & mask; while (pos > high);
        }
    }
    if (pos != 0) return -EINVAL;                                /* the spread must come back to its start */
    for (uint32_t u = 0; u < size; u++) {
        uint32_t ns = next[t->cell[u].sym]++;
        uint32_t nb = log - zstd_hbit(ns);
        t->cell[u].nbits = (uint8_t)nb;
        t->cell[u].base = (uint16_t)((ns << nb) - size);
    }
    t->log = log;
    return 0;
}

/* cnt (<= 24) bits at bit position `bit` of src[0, n); bits beyond the end read as zero */
static uint32_t fwd(const uint8_t *p, size_t n, uint64_t bit, uint32_t cnt)
{
    size_t i = (size_t)(bit >> 3);
    uint32_t v = 0;
    for (uint32_t k = 0; k < 4; k++)
        if (i + k < n) v |= (uint32_t)p[i + k] << (8 * k);
    return (v >> (bit & 7)) & ((1u << cnt) - 1);
}

int zstd_fse_read(FseTable *t, const uint8_t *src, size_t n, uint32_t max_log, uint32_t max_sym, size_t *used)
{
    int16_t norm[256];
    uint64_t bit = 4, limit = (uint64_t)n * 8;
    if (!n) return -EINVAL;
    uint32_t log = fwd(src, n, 0, 4) + 5;
    if (log > max_log) return -EINVAL;
    int32_t remaining = (1 << log) + 1, threshold = 1 << log;
    uint32_t nb = log + 1, sym = 0;
    bool prev0 = false;
    while (remaining > 1 && sym <= max_sym) {
        if (prev0) {                                             /* a run of unused symbols: 2-bit counts, 3 = "and more" */
            uint32_t n0 = sym, f;
            while ((f = fwd(src, n, bit, 2)) == 3) {
                n0 += 3;
                bit += 2;
                if (bit > limit) return -EINVAL;
            }
            n0 += f;
            bit += 2;
            if (n0 > max_sym) return -EINVAL;
            while (sym < n0) norm[sym++] = 0;
        }
        int32_t max = 2 * threshold - 1 - remaining, count;
        uint32_t v = fwd(src, n, bit, nb);
        if ((int32_t)(v & (uint32_t)(threshold - 1)) < max) {
            count = (int32_t)(v & (uint32_t)(threshold - 1));
            bit += nb - 1;
        } else {
            count = (int32_t)(v & (uint32_t)(2 * threshold - 1));
            if (count >= threshold) count -= max;
            bit += nb;
        }
        count--;                                                 /* stored +1: 0 means "less than one" */
        remaining -= count < 0 ? -count : count;
        norm[sym++] = (int16_t)count;
        prev0 = count == 0;
        while (remaining < threshold) {
            nb--;
            threshold >>= 1;
        }
        if (bit > limit) return -EINVAL;
    }
    if (remaining != 1 || bit > limit) return -EINVAL;
    *used = (size_t)((bit + 7) >> 3);
    return fse_build(t, norm, sym, log);
}

void zstd_fse_rle(FseTable *t, uint8_t sym)
{
    t->log = 0;
    t->cell[0].base = 0;
    t->cell[0].nbits = 0;
    t->cell[0].sym = sym;
}

int zstd_fse_predef(FseTable *t, int which)
{
    const int8_t *src = which == 0 ? ll_norm : which == 1 ? of_norm : ml_norm;
    uint32_t n = which == 0 ? 36 : which == 1 ? 29 : 53;
    int16_t norm[53];
    for (uint32_t i = 0; i < n; i++) norm[i] = src[i];
    return fse_build(t, norm, n, which == 1 ? 5 : 6);
}
