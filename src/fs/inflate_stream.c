/* src/fs/inflate_stream.c - streaming RFC 1951 DEFLATE (and PKZIP Deflate64) decoder for the archive layer (freestanding C23).
 *
 * The whole compressed stream is in memory (an mmap of the archive); the output leaves through a sink in 64 KiB pieces from a ring
 * window, so the RAM in use is one ~71 KiB block whatever the size of the data - a 500 MiB entry costs the same as a 5 KiB one.
 * Unlike inflate.c (which pads a short input with zero bits and leaves the verdict to its output cap) this decoder is strict:
 *   - the input ending inside the stream is -EINVAL (zero bits are fed, but a symbol that used them is noticed at once);
 *   - an over-subscribed code, an incomplete one (more than a single code), a dynamic block without an end-of-block code, a distance
 *     before the start of the output or beyond the window, and a reserved block type are -EINVAL;
 *   - the output cap max_out is exact: the sink never receives a byte beyond it, the result is -EFBIG.
 * Huffman codes decode through a 10-bit lookup table (codes up to 15 bits fall back to the canonical bit-by-bit walk). */
#include "duop_inflate.h"
#include "duop_oop.h"
#include "duop_pal.h"

#define WSIZE 65536u
#define WMASK (WSIZE - 1)
#define FAST_BITS 10
#define FAST_SIZE (1u << FAST_BITS)

typedef struct {
    uint16_t fast[FAST_SIZE];       /* (symbol << 4) | length of codes up to FAST_BITS bits, indexed by the next bits; 0 = none */
    uint16_t count[16];             /* codes per length */
    uint16_t symbol[320];           /* symbols ordered by (length, value) */
} Code;

typedef struct {
    const uint8_t *src, *src_end, *base;
    uint64_t bitbuf;
    uint32_t bitcnt;                /* bits held in bitbuf; bits above it may already hold the next real bytes */
    uint32_t virt;                  /* zero bytes fed after the end of the input (a symbol that used them means truncation) */
    uint32_t wpos;
    bool d64;
    uint64_t flushed, max_out;      /* bytes delivered; flushed <= max_out always */
    const CodecSink *sink;
    Code lit, dist;
    uint8_t win[WSIZE];             /* ring: the byte at output position p lives at win[p & WMASK] */
} Inf;

static const uint16_t len_base[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                                      35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
static const uint8_t len_ext[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static const uint32_t dist_base[32] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769,
                                       1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577, 32769, 49153};
static const uint8_t dist_ext[32] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13, 14, 14};
static const uint8_t cl_order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};

static inline void refill(Inf *st)
{
    if (st->src_end - st->src >= 8) {                           /* 8 bytes at once; only whole bytes are counted */
        st->bitbuf |= load_le64(st->src) << st->bitcnt;
        uint32_t adv = (63 - st->bitcnt) >> 3;
        st->src += adv;
        st->bitcnt += adv << 3;
        return;
    }
    while (st->bitcnt <= 56) {
        if (st->src < st->src_end) st->bitbuf |= (uint64_t)*st->src++ << st->bitcnt;
        else st->virt++;
        st->bitcnt += 8;
    }
}

static inline uint32_t take(Inf *st, uint32_t n)                /* n <= 16 and bitcnt >= n (a refill before) */
{
    uint32_t v = (uint32_t)(st->bitbuf & ((1ull << n) - 1));
    st->bitbuf >>= n;
    st->bitcnt -= n;
    return v;
}

static inline bool overran(const Inf *st) { return st->virt && st->bitcnt < st->virt * 8; }

/* delivers win[0, wpos) (at most up to max_out) and starts the next lap of the ring */
static intptr_t flush(Inf *st)
{
    uint32_t n = st->wpos;
    bool over = false;
    if (st->max_out - st->flushed < n) {
        n = (uint32_t)(st->max_out - st->flushed);
        over = true;
    }
    if (n) {
        intptr_t r = st->sink->write(st->sink->ctx, st->win, n);
        if (r < 0) return r;
        st->flushed += n;
    }
    if (over) return -EFBIG;
    st->wpos = 0;
    return 0;
}

/* canonical Huffman code from code lengths (each <= 15): -EINVAL when over-subscribed or incomplete (a single code may be) */
static int code_build(Code *c, const uint8_t *lens, uint32_t n)
{
    uint16_t offs[17], next[17];
    for (int i = 0; i < 16; i++) c->count[i] = 0;
    for (uint32_t i = 0; i < n; i++) c->count[lens[i]]++;
    c->count[0] = 0;
    int left = 1;
    uint32_t total = 0;
    for (int len = 1; len < 16; len++) {
        left <<= 1;
        left -= c->count[len];
        if (left < 0) return -EINVAL;
        total += c->count[len];
    }
    if (left > 0 && total > 1) return -EINVAL;
    uint32_t code = 0;
    offs[1] = 0;
    for (int len = 1; len < 16; len++) {
        code = (code + c->count[len - 1]) << 1;
        next[len] = (uint16_t)code;
        offs[len + 1] = (uint16_t)(offs[len] + c->count[len]);
    }
    for (uint32_t i = 0; i < FAST_SIZE; i++) c->fast[i] = 0;
    for (uint32_t sym = 0; sym < n; sym++) {
        uint32_t len = lens[sym];
        if (!len) continue;
        c->symbol[offs[len]++] = (uint16_t)sym;
        uint32_t cd = next[len]++, rev = 0;
        if (len > FAST_BITS) continue;
        for (uint32_t b = 0; b < len; b++) rev |= ((cd >> b) & 1u) << (len - 1 - b);   /* codes are packed MSB first */
        for (uint32_t j = rev; j < FAST_SIZE; j += 1u << len) c->fast[j] = (uint16_t)(sym << 4 | len);
    }
    return 0;
}

static int decode_slow(Inf *st, const Code *c)
{
    uint32_t bits = (uint32_t)st->bitbuf, code = 0, first = 0, index = 0;
    for (uint32_t len = 1; len < 16; len++) {
        code |= bits & 1u;
        bits >>= 1;
        uint32_t count = c->count[len];
        if (code < first + count) {
            st->bitbuf >>= len;
            st->bitcnt -= len;
            return c->symbol[index + (code - first)];
        }
        index += count;
        first = (first + count) << 1;
        code <<= 1;
    }
    return -1;
}

static inline int decode(Inf *st, const Code *c)
{
    uint32_t e = c->fast[st->bitbuf & (FAST_SIZE - 1)];
    if (e) {
        st->bitbuf >>= e & 15;
        st->bitcnt -= e & 15;
        return (int)(e >> 4);
    }
    return decode_slow(st, c);
}

static intptr_t copy_match(Inf *st, uint32_t dist, uint32_t len)
{
    uint32_t from = (st->wpos - dist) & WMASK;
    while (len) {
        uint32_t room = WSIZE - st->wpos, n = len < room ? len : room;
        for (uint32_t i = 0; i < n; i++) st->win[st->wpos + i] = st->win[(from + i) & WMASK];   /* forward: overlap repeats */
        st->wpos += n;
        from = (from + n) & WMASK;
        len -= n;
        if (st->wpos == WSIZE) {
            intptr_t r = flush(st);
            if (r) return r;
        }
    }
    return 0;
}

static intptr_t block_huff(Inf *st)
{
    const uint32_t ndist = st->d64 ? 32 : 30;
    for (;;) {
        refill(st);
        int s = decode(st, &st->lit);
        if (s < 0 || overran(st)) return -EINVAL;
        if (s < 256) {
            st->win[st->wpos++] = (uint8_t)s;
            if (st->wpos == WSIZE) {
                intptr_t r = flush(st);
                if (r) return r;
            }
            continue;
        }
        if (s == 256) return 0;
        s -= 257;
        if (s >= 29) return -EINVAL;
        uint32_t len = len_base[s];
        if (st->d64 && s == 28) len = 3 + take(st, 16);
        else if (len_ext[s]) len += take(st, len_ext[s]);
        refill(st);
        int d = decode(st, &st->dist);
        if (d < 0 || (uint32_t)d >= ndist) return -EINVAL;
        uint32_t dist = dist_base[d] + (dist_ext[d] ? take(st, dist_ext[d]) : 0);
        if (overran(st)) return -EINVAL;
        if (dist > st->flushed + st->wpos) return -EINVAL;                     /* before the first byte (a code cannot reach beyond the window) */
        intptr_t r = copy_match(st, dist, len);
        if (r) return r;
    }
}

static intptr_t block_stored(Inf *st)
{
    uint32_t drop = st->bitcnt & 7;
    st->bitbuf >>= drop;
    st->bitcnt -= drop;
    int64_t real = (int64_t)(st->bitcnt >> 3) - (int64_t)st->virt;             /* whole real bytes still in the bit buffer */
    if (real < 0) return -EINVAL;
    st->src -= real;
    st->bitbuf = 0;
    st->bitcnt = 0;
    st->virt = 0;
    if (st->src_end - st->src < 4) return -EINVAL;
    uint32_t len = load_le16(st->src), nlen = load_le16(st->src + 2);
    st->src += 4;
    if ((~nlen & 0xFFFFu) != len) return -EINVAL;
    if (len > (uint64_t)(st->src_end - st->src)) return -EINVAL;
    while (len) {
        uint32_t room = WSIZE - st->wpos, n = len < room ? len : room;
        mem_copy(st->win + st->wpos, st->src, n);
        st->wpos += n;
        st->src += n;
        len -= n;
        if (st->wpos == WSIZE) {
            intptr_t r = flush(st);
            if (r) return r;
        }
    }
    return 0;
}

static intptr_t build_fixed(Inf *st)
{
    uint8_t l[288];
    int i = 0;
    for (; i < 144; i++) l[i] = 8;
    for (; i < 256; i++) l[i] = 9;
    for (; i < 280; i++) l[i] = 7;
    for (; i < 288; i++) l[i] = 8;
    if (code_build(&st->lit, l, 288) < 0) return -EINVAL;
    for (i = 0; i < 32; i++) l[i] = 5;
    return code_build(&st->dist, l, 32) < 0 ? -EINVAL : 0;
}

/* the code-length symbols of a dynamic header: total lengths into lens[] */
static intptr_t read_lengths(Inf *st, uint8_t *lens, uint32_t total)
{
    uint32_t n = 0;
    while (n < total) {
        refill(st);
        int s = decode(st, &st->lit);                                          /* the code-length code lives in the lit slot */
        if (s < 0 || overran(st)) return -EINVAL;
        if (s < 16) {
            lens[n++] = (uint8_t)s;
            continue;
        }
        uint32_t rep;
        uint8_t v = 0;
        if (s == 16) {
            if (!n) return -EINVAL;
            rep = 3 + take(st, 2);
            v = lens[n - 1];
        } else if (s == 17) {
            rep = 3 + take(st, 3);
        } else {
            rep = 11 + take(st, 7);
        }
        if (overran(st) || rep > total - n) return -EINVAL;
        while (rep--) lens[n++] = v;
    }
    return 0;
}

static intptr_t load_dynamic(Inf *st)
{
    uint8_t lens[320];
    for (int i = 0; i < 320; i++) lens[i] = 0;
    refill(st);
    uint32_t hlit = take(st, 5) + 257, hdist = take(st, 5) + 1, hclen = take(st, 4) + 4;
    if (hlit > 286 || hdist > (st->d64 ? 32u : 30u)) return -EINVAL;
    for (uint32_t i = 0; i < hclen; i++) {
        refill(st);
        lens[cl_order[i]] = (uint8_t)take(st, 3);
    }
    if (overran(st) || code_build(&st->lit, lens, 19) < 0) return -EINVAL;
    uint8_t all[320];
    for (int i = 0; i < 320; i++) all[i] = 0;
    intptr_t r = read_lengths(st, all, hlit + hdist);
    if (r) return r;
    if (!all[256]) return -EINVAL;                                             /* no end-of-block code */
    if (code_build(&st->lit, all, hlit) < 0 || code_build(&st->dist, all + hlit, hdist) < 0) return -EINVAL;
    return 0;
}

static intptr_t inflate_blocks(Inf *st)
{
    bool last;
    do {
        refill(st);
        last = take(st, 1) != 0;
        uint32_t type = take(st, 2);
        intptr_t r = -EINVAL;
        if (type == 0) {
            r = block_stored(st);
        } else if (type == 1) {
            r = build_fixed(st);
            if (!r) r = block_huff(st);
        } else if (type == 2) {
            r = load_dynamic(st);
            if (!r) r = block_huff(st);
        }
        if (r) return r;
        if (overran(st)) return -EINVAL;
    } while (!last);
    return flush(st);
}

intptr_t inflate_stream(const void *src, uint64_t src_len, uint32_t flags, uint64_t max_out, const CodecSink *sink, CodecResult *res)
{
    res->consumed = res->produced = 0;
    Inf *st = mem_alloc(sizeof *st);
    if (!st) return -ENOMEM;
    st->src = st->base = src;
    st->src_end = (const uint8_t *)src + src_len;
    st->bitbuf = 0;
    st->bitcnt = st->virt = st->wpos = 0;
    st->d64 = flags & INFLATE_DEFLATE64;
    st->flushed = 0;
    st->max_out = max_out;
    st->sink = sink;
    intptr_t r = inflate_blocks(st);
    res->produced = st->flushed;
    if (r == 0) {
        int64_t real = (int64_t)(st->bitcnt >> 3) - (int64_t)st->virt;         /* whole unread bytes behind the final block */
        if (real < 0) r = -EINVAL;
        else res->consumed = (uint64_t)(st->src - st->base) - (uint64_t)real;
    }
    mem_free(st);
    return r;
}
