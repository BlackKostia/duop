/* src/fs/zstd_seq.c - the sequences section of a Zstandard block (RFC 8878 section 3.1.1.3.2; freestanding C23).
 *
 * Three FSE tables (literal lengths, offsets, match lengths) drive one backward bitstream. Each sequence is copy `ll` literals, then
 * copy `ml` bytes from `offset` back in the history; offsets 1..3 mean "one of the three most recent offsets". The block is
 * untrusted: every length is checked against the literals left and the room of the block, an offset against the history that
 * exists and the frame window, and the bitstream must end exactly where the last sequence ends. */
#include "duop_zstd_int.h"

static const uint8_t ll_bits[36] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 3, 3, 4, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
static const uint32_t ll_base[36] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 18, 20, 22, 24, 28, 32, 40, 48, 64, 128, 256, 512, 1024, 2048,
                                     4096, 8192, 16384, 32768, 65536};
static const uint8_t ml_bits[53] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                    1, 1, 1, 1, 2, 2, 3, 3, 4, 4, 5, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
static const uint32_t ml_base[53] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34,
                                     35, 37, 39, 41, 43, 47, 51, 59, 67, 83, 99, 131, 259, 515, 1027, 2051, 4099, 8195, 16387, 32771, 65539};

/* one of the three tables as the mode says: predefined, one repeated symbol, described here, or the previous one again */
static int table_for(ZstdCtx *c, FseTable *t, uint32_t flag, uint32_t mode, int which, const uint8_t *src, size_t n, size_t *pos)
{
    uint32_t max_log = which == 1 ? 8 : 9, max_sym = which == 0 ? 35 : which == 1 ? 31 : 52;
    if (mode == 0) {
        int r = zstd_fse_predef(t, which);
        if (r) return r;
    } else if (mode == 1) {
        if (*pos >= n || src[*pos] > max_sym) return -EINVAL;
        zstd_fse_rle(t, src[(*pos)++]);
    } else if (mode == 2) {
        size_t used;
        int r = zstd_fse_read(t, src + *pos, n - *pos, max_log, max_sym, &used);
        if (r) return r;
        *pos += used;
    } else if (!(c->have & flag)) {
        return -EINVAL;                                           /* Repeat_Mode before any table was set */
    }
    c->have |= flag;
    return 0;
}

/* the sequence count at the start of the section: bytes used, or 0 on a truncated header */
static size_t seq_count(const uint8_t *src, size_t n, uint32_t *count)
{
    uint32_t b0 = src[0];
    if (b0 < 128) {
        *count = b0;
        return 1;
    }
    if (b0 < 255) {
        if (n < 2) return 0;
        *count = ((b0 - 128) << 8) + src[1];
        return 2;
    }
    if (n < 3) return 0;
    *count = (uint32_t)src[1] + ((uint32_t)src[2] << 8) + 0x7F00;
    return 3;
}

/* the offset of a sequence from its code value (and the repeat offsets updated): 0 when it is invalid */
static uint32_t pick_offset(ZstdCtx *c, uint64_t ofv, uint32_t ll)
{
    uint32_t *rep = c->rep, offset;
    if (ofv > 3) {
        if (ofv - 3 > UINT32_MAX) return 0;
        offset = (uint32_t)(ofv - 3);
        rep[2] = rep[1];
        rep[1] = rep[0];
        rep[0] = offset;
        return offset;
    }
    uint32_t idx = (uint32_t)ofv - 1 + (ll == 0);                 /* with no literals the codes shift: 1 is the second repeat offset */
    if (idx == 0) return rep[0];
    offset = idx == 3 ? rep[0] - 1 : rep[idx];
    if (!offset) {
        if (ZSTD_STRICT) return 0;                                 /* the format calls it corruption; libzstd takes it as offset 1 */
        offset = 1;
    }
    if (idx >= 2) rep[2] = rep[1];
    rep[1] = rep[0];
    rep[0] = offset;
    return offset;
}

/* copy ll literals, then ml bytes from `offset` back */
static int run_one(ZstdCtx *c, size_t *lpos, uint32_t ll, uint32_t ml, uint32_t offset)
{
    if (!offset || ll > c->lit_len - *lpos || (uint64_t)ll + ml > c->limit - c->pos) return -EINVAL;
    __builtin_memcpy(c->win + c->pos, c->lit + *lpos, ll);
    c->pos += ll;
    *lpos += ll;
    if (offset > c->pos || offset > c->window) return -EINVAL;     /* before the start of the history, or beyond the window */
    uint8_t *d = c->win + c->pos;
    const uint8_t *s = d - offset;
    if (offset >= ml) {
        __builtin_memcpy(d, s, ml);
    } else {
        for (uint32_t k = 0; k < ml; k++) d[k] = s[k];             /* overlapping: a pattern repeated */
    }
    c->pos += ml;
    return 0;
}

static int decode_all(ZstdCtx *c, Rev *b, uint32_t nseq, size_t *lpos)
{
    uint32_t sll = (uint32_t)rev_read(b, c->ll.log), sof = (uint32_t)rev_read(b, c->of.log), sml = (uint32_t)rev_read(b, c->ml.log);
    for (uint32_t i = 0; i < nseq; i++) {
        const FseCell *cl = &c->ll.cell[sll], *co = &c->of.cell[sof], *cm = &c->ml.cell[sml];
        (void)rev_reload(b);
        uint64_t ofv = ((uint64_t)1 << co->sym) + rev_read(b, co->sym);
        uint32_t ml = ml_base[cm->sym] + (uint32_t)rev_read(b, ml_bits[cm->sym]);
        (void)rev_reload(b);
        uint32_t ll = ll_base[cl->sym] + (uint32_t)rev_read(b, ll_bits[cl->sym]);
        if (i + 1 < nseq) {                                        /* the next states: literal lengths, match lengths, offsets */
            sll = cl->base + (uint32_t)rev_read(b, cl->nbits);
            sml = cm->base + (uint32_t)rev_read(b, cm->nbits);
            sof = co->base + (uint32_t)rev_read(b, co->nbits);
        }
        int r = run_one(c, lpos, ll, ml, pick_offset(c, ofv, ll));
        if (r) return r;
        if (b->used > 64) return -EINVAL;                          /* read past the front of the stream */
    }
    (void)rev_reload(b);
    return rev_finished(b) ? 0 : -EINVAL;
}

int zstd_sequences(ZstdCtx *c, const uint8_t *src, size_t n)
{
    uint32_t nseq = 0;
    size_t pos = 0, lpos = 0;
    if (!n) return -EINVAL;
    pos = seq_count(src, n, &nseq);
    if (!pos) return -EINVAL;
    if (nseq) {
        if (pos >= n) return -EINVAL;
        uint32_t modes = src[pos++];
        if (ZSTD_STRICT && (modes & 3)) return -EINVAL;           /* reserved bits */
        int r = table_for(c, &c->ll, ZSTD_HAVE_LL, modes >> 6, 0, src, n, &pos);
        if (!r) r = table_for(c, &c->of, ZSTD_HAVE_OF, (modes >> 4) & 3, 1, src, n, &pos);
        if (!r) r = table_for(c, &c->ml, ZSTD_HAVE_ML, (modes >> 2) & 3, 2, src, n, &pos);
        if (r) return r;
        Rev b;
        if (rev_init(&b, src + pos, n - pos)) return -EINVAL;
        r = decode_all(c, &b, nseq, &lpos);
        if (r) return r;
    } else if (pos != n) {
        return -EINVAL;                                            /* no sequences: nothing may follow */
    }
    size_t rest = c->lit_len - lpos;                               /* the literals after the last sequence */
    if (rest > c->limit - c->pos) return -EINVAL;
    __builtin_memcpy(c->win + c->pos, c->lit + lpos, rest);
    c->pos += rest;
    return 0;
}
