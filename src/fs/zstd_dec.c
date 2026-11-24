/* src/fs/zstd_dec.c - Zstandard frames and blocks (RFC 8878; freestanding C23).
 *
 * The compressed stream is wholly in memory; the output of every block goes to the sink as soon as it is complete, from a history
 * window that holds the last Window_Size bytes (a frame with a known content size that fits needs no more than that size). When the
 * buffer is full the last window is moved to its front - one copy per window of output, amortised - so the RAM in use depends on
 * the window of the frame (at most 2^27), never on the size of the data. Frame content sizes and checksums are verified.
 * Entropy coding lives in zstd_fse.c (tables), zstd_huf.c (literals) and zstd_seq.c (sequences). */
#include "duop_oop.h"
#include "duop_pal.h"
#include "duop_zstd_int.h"

typedef struct FrameInfo {
    uint64_t window, fcs;              /* Window_Size; Frame_Content_Size when has_fcs */
    bool has_fcs, checksum;
} FrameInfo;

typedef struct Dec {
    ZstdCtx *c;
    const CodecSink *sink;
    uint64_t max_out, produced;        /* the cap over all frames, and what the sink has received */
    Xxh64 xxh;
} Dec;

/* the frame header after the magic: its length, or -EINVAL / -EOPNOTSUPP (a dictionary, a window beyond the limit) */
static int64_t frame_header(const uint8_t *p, size_t n, FrameInfo *f)
{
    static const uint8_t dict_len[4] = {0, 1, 2, 4};
    if (n < 1) return -EINVAL;
    uint32_t d = p[0];
    size_t i = 1;
    if (d & 0x08) return -EINVAL;                                  /* a reserved bit */
    uint32_t fcsf = d >> 6, dictf = d & 3;
    bool single = (d >> 5) & 1;
    uint64_t window = 0;
    f->checksum = (d >> 2) & 1;
    if (!single) {
        if (i >= n) return -EINVAL;
        uint32_t wd = p[i++];
        uint64_t base = 1ull << (10 + (wd >> 3));
        window = base + (base >> 3) * (wd & 7);
    }
    size_t dn = dict_len[dictf];
    if (n - i < dn) return -EINVAL;
    uint32_t dict_id = 0;
    for (size_t k = 0; k < dn; k++) dict_id |= (uint32_t)p[i + k] << (8 * k);
    i += dn;
    size_t fn = fcsf ? (size_t)1 << fcsf : single;
    if (n - i < fn) return -EINVAL;
    uint64_t fcs = 0;
    for (size_t k = 0; k < fn; k++) fcs |= (uint64_t)p[i + k] << (8 * k);
    if (fcsf == 1) fcs += 256;
    i += fn;
    f->has_fcs = fn > 0;
    f->fcs = fcs;
    f->window = single ? fcs : window;
    if (dict_id) return -EOPNOTSUPP;                               /* a dictionary the stream does not carry */
    if (f->window > (1ull << ZSTD_WINDOW_LOG_LIMIT)) return -EOPNOTSUPP;
    return (int64_t)i;
}

/* the literals section of a compressed block into c->lit: 0, -EINVAL; *used = its bytes */
static int literals(ZstdCtx *c, const uint8_t *src, size_t n, size_t *used)
{
    if (!n) return -EINVAL;
    uint32_t type = src[0] & 3, fmt = (src[0] >> 2) & 3;
    if (type < 2) {                                                /* Raw or RLE: a size of 5, 12 or 20 bits */
        size_t hdr = fmt == 1 ? 2 : fmt == 3 ? 3 : 1, reg;
        if (n < hdr) return -EINVAL;
        reg = hdr == 1 ? src[0] >> 3 : hdr == 2 ? (size_t)(src[0] >> 4) + ((size_t)src[1] << 4) : (size_t)(src[0] >> 4) + ((size_t)src[1] << 4) + ((size_t)src[2] << 12);
        size_t take = type == 0 ? reg : 1;
        if (reg > ZSTD_BLOCK_MAX || n - hdr < take) return -EINVAL;
        if (type == 0) __builtin_memcpy(c->lit, src + hdr, reg);
        else __builtin_memset(c->lit, src[hdr], reg);
        c->lit_len = reg;
        *used = hdr + take;
        return 0;
    }
    size_t hdr = fmt < 2 ? 3 : fmt == 2 ? 4 : 5, bits = fmt < 2 ? 10 : fmt == 2 ? 14 : 18;
    if (n < hdr) return -EINVAL;
    uint64_t v = 0;
    for (size_t k = 0; k < hdr; k++) v |= (uint64_t)src[k] << (8 * k);
    uint64_t mask = (1ull << bits) - 1, reg = (v >> 4) & mask, comp = (v >> (4 + bits)) & mask;
    if (reg > ZSTD_BLOCK_MAX || comp > n - hdr) return -EINVAL;
    const uint8_t *p = src + hdr;
    size_t tree = 0;
    if (type == 2) {                                               /* with a new tree */
        int r = zstd_huf_table(&c->huf, p, (size_t)comp, &tree);
        if (r) return r;
        c->have |= ZSTD_HAVE_HUF;
    } else if (!(c->have & ZSTD_HAVE_HUF)) {
        return -EINVAL;                                            /* Treeless: the previous tree is reused, and there is none */
    }
    int r = zstd_huf_decode(&c->huf, c->lit, (size_t)reg, p + tree, (size_t)comp - tree, fmt != 0);
    if (r) return r;
    c->lit_len = (size_t)reg;
    *used = hdr + (size_t)comp;
    return 0;
}

/* hands [done, pos) to the sink (at most CODEC_CHUNK at a time, never beyond max_out) and hashes it */
static intptr_t deliver(Dec *d, bool check)
{
    ZstdCtx *c = d->c;
    size_t n = c->pos - c->done;
    const uint8_t *p = c->win + c->done;
    if (check) xxh64_update(&d->xxh, p, n);
    c->total += n;
    while (n) {
        size_t k = n < CODEC_CHUNK ? n : CODEC_CHUNK;
        uint64_t room = d->max_out - d->produced;
        bool over = k > room;
        if (over) k = (size_t)room;
        if (k) {
            intptr_t r = d->sink->write(d->sink->ctx, p, k);
            if (r < 0) return r;
            d->produced += k;
        }
        if (over) return -EFBIG;
        p += k;
        n -= k;
    }
    c->done = c->pos;
    return 0;
}

static int run_block(ZstdCtx *c, uint32_t type, size_t size, const uint8_t *src)
{
    if (type == 0) {
        __builtin_memcpy(c->win + c->pos, src, size);
        c->pos += size;
    } else if (type == 1) {
        __builtin_memset(c->win + c->pos, src[0], size);
        c->pos += size;
    } else {
        size_t used;
        int r = literals(c, src, size, &used);
        if (!r) r = zstd_sequences(c, src + used, size - used);
        if (r) return r;
    }
    return 0;
}

/* the blocks of one frame up to the last: 0 or an error; *used = bytes of the blocks */
static int blocks(Dec *d, const FrameInfo *f, const uint8_t *p, size_t n, size_t *used)
{
    ZstdCtx *c = d->c;
    size_t i = 0, max_block = c->window < ZSTD_BLOCK_MAX ? (size_t)c->window : ZSTD_BLOCK_MAX;
    for (;;) {
        if (n - i < 3) return -EINVAL;
        uint32_t bh = (uint32_t)p[i] | (uint32_t)p[i + 1] << 8 | (uint32_t)p[i + 2] << 16;
        uint32_t type = (bh >> 1) & 3;
        size_t size = bh >> 3, in = type == 1 ? 1 : size;
        i += 3;
        if (type == 3 || size > max_block || n - i < in) return -EINVAL;
        if (c->cap - c->pos < max_block) {                         /* full: keep the last window at the front */
            size_t keep = c->pos < c->window ? c->pos : (size_t)c->window;
            __builtin_memmove(c->win, c->win + c->pos - keep, keep);
            c->pos = c->done = keep;
            if (c->cap - c->pos < max_block) return -EINVAL;
        }
        c->limit = c->pos + max_block;
        int r = run_block(c, type, size, p + i);
        if (r) return r;
        i += in;
        intptr_t w = deliver(d, f->checksum);
        if (w) return (int)w;
        if (f->has_fcs && c->total > f->fcs) return -EINVAL;
        if (bh & 1) break;
    }
    *used = i;
    return 0;
}

static int decode_frame(Dec *d, const uint8_t *p, size_t n, size_t *used)
{
    FrameInfo f;
    int64_t h = frame_header(p, n, &f);
    if (h < 0) return (int)h;
    ZstdCtx *c = d->c;
    uint64_t mb = f.window < ZSTD_BLOCK_MAX ? f.window : ZSTD_BLOCK_MAX, extra = f.window > (1u << 20) ? f.window : (1u << 20);
    uint64_t cap = f.window + extra + mb;
    if (f.has_fcs && f.fcs + mb < cap) cap = f.fcs + mb;           /* it all fits: the history never has to move */
    if (cap < 64) cap = 64;                                        /* an empty single-segment frame still gets a buffer */
    c->win = mem_alloc((size_t)cap);
    if (!c->win) return -ENOMEM;
    c->cap = (size_t)cap;
    c->pos = c->done = 0;
    c->total = 0;
    c->window = f.window;
    c->have = 0;
    c->rep[0] = 1;
    c->rep[1] = 4;
    c->rep[2] = 8;
    xxh64_init(&d->xxh);
    size_t used_blocks = 0;
    int r = blocks(d, &f, p + h, n - (size_t)h, &used_blocks);
    size_t i = (size_t)h + used_blocks;
    if (!r && f.checksum) {
        if (n - i < 4) r = -EINVAL;
        else if (load_le32(p + i) != (uint32_t)xxh64_digest(&d->xxh)) r = -EBADMSG;
        i += 4;
    }
    if (!r && f.has_fcs && c->total != f.fcs) r = -EINVAL;
    mem_free(c->win);
    c->win = nullptr;
    *used = i;
    return r;
}

intptr_t zstd_decode(const void *src, uint64_t src_len, uint64_t max_out, const CodecSink *sink, CodecResult *res)
{
    res->consumed = res->produced = 0;
    Dec *d = mem_alloc(sizeof *d);
    ZstdCtx *c = mem_alloc(sizeof *c);
    uint8_t *lit = mem_alloc(ZSTD_BLOCK_MAX + 32);
    if (!d || !c || !lit) {
        mem_free(d);
        mem_free(c);
        mem_free(lit);
        return -ENOMEM;
    }
    d->c = c;
    d->sink = sink;
    d->max_out = max_out;
    d->produced = 0;
    c->lit = lit;
    const uint8_t *p = src;
    uint64_t pos = 0;
    intptr_t rc = src_len ? 0 : -EINVAL;
    while (!rc && pos < src_len) {
        uint64_t n = src_len - pos;
        uint32_t magic = n >= 4 ? load_le32(p + pos) : 0;
        if ((magic & 0xFFFFFFF0u) == 0x184D2A50u) {                /* a skippable frame: its size, then that many bytes to ignore */
            if (n < 8 || load_le32(p + pos + 4) > n - 8) rc = -EINVAL;
            else pos += 8 + (uint64_t)load_le32(p + pos + 4);
        } else if (magic != ZSTD_MAGIC) {
            rc = -EINVAL;
        } else {
            size_t used = 0;
            rc = decode_frame(d, p + pos + 4, (size_t)(n - 4), &used);
            if (!rc) pos += 4 + used;
        }
        res->consumed = rc ? res->consumed : pos;
    }
    res->produced = d->produced;
    mem_free(c->win);
    mem_free(lit);
    mem_free(c);
    mem_free(d);
    return rc;
}
