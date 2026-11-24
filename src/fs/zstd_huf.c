/* src/fs/zstd_huf.c - the Huffman coded literals of Zstandard (RFC 8878 section 4.2; freestanding C23).
 *
 * The tree is given as weights (4 bits each, or themselves FSE coded with two interleaved states); the weight of the last symbol is
 * implied by the rest adding up to a power of two. Symbols are decoded through a flat table indexed by the next `log` bits. The
 * literals come in one stream or four (a jump table gives the sizes of the first three); each stream is read backwards and must be
 * used up exactly - a short or long stream is -EINVAL, never an out-of-range read. */
#include "duop_zstd_int.h"

enum { MAX_WEIGHTS = 255 };

/* the weights coded with FSE: one backward stream, two states taking turns (the stream ends when a read runs past its front) */
static int weights_fse(uint8_t *w, uint32_t *nw, const uint8_t *src, size_t n)
{
    FseTable ft;
    size_t used;
    int r = zstd_fse_read(&ft, src, n, 6, 12, &used);
    if (r) return r;
    Rev b;
    if (rev_init(&b, src + used, n - used)) return -EINVAL;
    uint32_t s1 = (uint32_t)rev_read(&b, ft.log), s2 = (uint32_t)rev_read(&b, ft.log), cnt = 0;
    for (;;) {
        if (cnt > MAX_WEIGHTS - 2) return -EINVAL;
        const FseCell *c = &ft.cell[s1];
        w[cnt++] = c->sym;
        s1 = c->base + (uint32_t)rev_read(&b, c->nbits);
        if (rev_reload(&b) == REV_OVER) {
            w[cnt++] = ft.cell[s2].sym;
            break;
        }
        c = &ft.cell[s2];
        w[cnt++] = c->sym;
        s2 = c->base + (uint32_t)rev_read(&b, c->nbits);
        if (rev_reload(&b) == REV_OVER) {
            w[cnt++] = ft.cell[s1].sym;
            break;
        }
    }
    *nw = cnt;
    return 0;
}

int zstd_huf_table(HufTable *t, const uint8_t *src, size_t n, size_t *used)
{
    uint8_t w[MAX_WEIGHTS + 2];
    uint32_t nw;
    if (!n) return -EINVAL;
    uint32_t hb = src[0];
    if (hb >= 128) {                                             /* the weights as they are, two to a byte */
        nw = hb - 127;
        size_t nbytes = (nw + 1) / 2;
        if (1 + nbytes > n) return -EINVAL;
        for (uint32_t i = 0; i < nw; i++) w[i] = (i & 1) ? src[1 + i / 2] & 15 : src[1 + i / 2] >> 4;
        *used = 1 + nbytes;
    } else {
        if (!hb || 1 + (size_t)hb > n) return -EINVAL;
        int r = weights_fse(w, &nw, src + 1, hb);
        if (r) return r;
        *used = 1 + (size_t)hb;
    }
    uint32_t sum = 0;
    for (uint32_t i = 0; i < nw; i++) {
        if (w[i] > ZSTD_HUF_LOG) return -EINVAL;
        if (w[i]) sum += 1u << (w[i] - 1);
    }
    if (!sum) return -EINVAL;
    uint32_t mb = zstd_hbit(sum) + 1;                            /* the code lengths: the total is the next power of two ... */
    if (mb > ZSTD_HUF_LOG) return -EINVAL;
    uint32_t left = (1u << mb) - sum;
    if (left & (left - 1)) return -EINVAL;                       /* ... and what is missing is the last symbol, a power of two */
    w[nw++] = (uint8_t)(zstd_hbit(left) + 1);
    uint32_t count[ZSTD_HUF_LOG + 2] = {0}, start[ZSTD_HUF_LOG + 2] = {0}, acc = 0;
    for (uint32_t i = 0; i < nw; i++) count[w[i]]++;
    for (uint32_t k = 1; k <= mb; k++) {                         /* short weights (long codes) first */
        start[k] = acc;
        acc += count[k] << (k - 1);
    }
    for (uint32_t i = 0; i < nw; i++) {
        if (!w[i]) continue;
        uint32_t k = w[i], len = 1u << (k - 1);
        for (uint32_t u = start[k]; u < start[k] + len; u++) {
            t->cell[u].sym = (uint8_t)i;
            t->cell[u].nbits = (uint8_t)(mb + 1 - k);
        }
        start[k] += len;
    }
    t->log = mb;
    return 0;
}

static int decode_stream(const HufTable *t, uint8_t *dst, size_t len, const uint8_t *src, size_t n)
{
    Rev b;
    if (rev_init(&b, src, n)) return -EINVAL;
    uint32_t log = t->log;
    size_t i = 0;
    while (i < len) {
        (void)rev_reload(&b);                                     /* at least 57 bits while the stream lasts: four symbols of <= 12 */
        size_t k = len - i < 4 ? len - i : 4;
        for (size_t j = 0; j < k; j++) {
            HufCell c = t->cell[rev_look(&b, log)];
            dst[i++] = c.sym;
            b.used += c.nbits;
        }
    }
    (void)rev_reload(&b);
    return rev_finished(&b) || !ZSTD_STRICT ? 0 : -EINVAL;      /* every bit used: neither too long nor too short */
}

int zstd_huf_decode(const HufTable *t, uint8_t *dst, size_t dst_len, const uint8_t *src, size_t n, bool four)
{
    if (!four) return decode_stream(t, dst, dst_len, src, n);
    if (n < 10) return -EINVAL;
    size_t s1 = load_le16(src), s2 = load_le16(src + 2), s3 = load_le16(src + 4);
    if (s1 + s2 + s3 + 6 >= n) return -EINVAL;                   /* the fourth stream is the rest and cannot be empty */
    size_t s4 = n - 6 - s1 - s2 - s3, seg = (dst_len + 3) / 4;
    if (seg * 3 > dst_len) return -EINVAL;                       /* under 6 literals cannot use four streams */
    const uint8_t *p = src + 6;
    int r = decode_stream(t, dst, seg, p, s1);
    if (!r) r = decode_stream(t, dst + seg, seg, p + s1, s2);
    if (!r) r = decode_stream(t, dst + 2 * seg, seg, p + s1 + s2, s3);
    if (!r) r = decode_stream(t, dst + 3 * seg, dst_len - 3 * seg, p + s1 + s2 + s3, s4);
    return r;
}
