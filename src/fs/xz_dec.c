/* src/fs/xz_dec.c - the .xz container (freestanding C23): stream header, blocks, index, footer, padding, integrity checks.
 *
 * Streams are decoded one after another from the whole input in memory. Every field is verified as liblzma verifies it: the CRC-32 of
 * the stream flags and of each block header, reserved bits, the sizes the block header may state, the zero padding of the block and of
 * the stream, the check of the uncompressed data (CRC-32, CRC-64, SHA-256; a type of check that is not known is skipped, as `xz` does),
 * and the index and footer against the blocks that were really decoded. The filter chain must be LZMA2 alone: BCJ and Delta are
 * known but not implemented (-EOPNOTSUPP), anything else is -EINVAL. */
#include "duop_hash.h"
#include "duop_oop.h"
#include "duop_pal.h"
#include "duop_xz_int.h"

enum { CHECK_NONE = 0, CHECK_CRC32 = 1, CHECK_CRC64 = 4, CHECK_SHA256 = 10, FILTER_LZMA2 = 0x21, MAX_BLOCK_HEADER = 1024 };

typedef struct Check {
    uint32_t kind;
    uint32_t crc32;
    uint64_t crc64;
    ShaCtx sha;
} Check;

static void check_feed(void *ctx, const uint8_t *p, size_t n)
{
    Check *c = ctx;
    if (c->kind == CHECK_CRC32) c->crc32 = crc32_update(c->crc32, p, n);
    else if (c->kind == CHECK_CRC64) c->crc64 = crc64_update(c->crc64, p, n);
    else if (c->kind == CHECK_SHA256) sha256_update(&c->sha, p, n);
}

static void check_begin(Check *c, uint32_t kind)
{
    c->kind = kind;
    c->crc32 = 0xFFFFFFFFu;
    c->crc64 = ~(uint64_t)0;
    if (kind == CHECK_SHA256) sha256_init(&c->sha);
}

/* the size in bytes of the check field of a type: 0 none, then 4, 8, 16, 32, 64 for ids 1-3, 4-6, 7-9, 10-12, 13-15 */
static size_t check_size(uint32_t id) { return id == 0 ? 0 : (size_t)4 << ((id - 1) / 3); }

static bool check_ok(Check *c, const uint8_t *field)
{
    if (c->kind == CHECK_CRC32) return load_le32(field) == ~c->crc32;
    if (c->kind == CHECK_CRC64) return load_le64(field) == ~c->crc64;
    if (c->kind != CHECK_SHA256) return true;                       /* none, or a type that is not verified */
    char hex[65], want[65];
    static const char digits[] = "0123456789abcdef";
    sha256_final_hex(&c->sha, hex);
    for (int i = 0; i < 32; i++) {
        want[2 * i] = digits[field[i] >> 4];
        want[2 * i + 1] = digits[field[i] & 15];
    }
    want[64] = 0;
    return !__builtin_memcmp(hex, want, 65);
}

/* a variable length integer (7 bits per byte, little endian, at most 9 bytes, no trailing zero byte): its length, or 0 */
static size_t vli(const uint8_t *p, size_t n, uint64_t *out)
{
    uint64_t v = 0;
    for (size_t i = 0; i < n && i < 9; i++) {
        v |= (uint64_t)(p[i] & 0x7F) << (7 * i);
        if (!(p[i] & 0x80)) {
            if (i && !p[i]) return 0;
            *out = v;
            return i + 1;
        }
    }
    return 0;
}

typedef struct BlockHdr {
    size_t size;                       /* bytes of the header */
    bool has_c, has_u;
    uint64_t csize, usize;
    uint32_t dict;
} BlockHdr;

/* the LZMA2 filter only: BCJ / Delta are -EOPNOTSUPP, other ids -EINVAL */
static int filter_chain(const uint8_t *p, size_t end, uint32_t nf, BlockHdr *h, size_t pos)
{
    for (uint32_t k = 0; k < nf; k++) {
        uint64_t id, ps;
        size_t a = vli(p + pos, end - pos, &id);
        if (!a) return -EINVAL;
        pos += a;
        a = vli(p + pos, end - pos, &ps);
        if (!a || ps > end - pos - a) return -EINVAL;
        pos += a;
        if (id == FILTER_LZMA2) {
            if (k != nf - 1 || ps != 1 || p[pos] > 40) return -EINVAL;
            h->dict = p[pos] == 40 ? 0xFFFFFFFFu : (2u | (p[pos] & 1u)) << (p[pos] / 2 + 11);
        } else if (id == 3 || (id >= 4 && id <= 0xB)) {
            if (k == nf - 1) return -EINVAL;
            return -EOPNOTSUPP;
        } else {
            return -EINVAL;
        }
        pos += ps;
    }
    for (; pos < end; pos++)
        if (p[pos]) return -EINVAL;                                 /* the header padding is zero */
    return 0;
}

static int block_header(const uint8_t *p, size_t n, BlockHdr *h)
{
    h->size = ((size_t)p[0] + 1) * 4;
    if (h->size < 8 || h->size > n || h->size > MAX_BLOCK_HEADER) return -EINVAL;
    if (load_le32(p + h->size - 4) != ~crc32_update(0xFFFFFFFFu, p, h->size - 4)) return -EINVAL;
    uint32_t flags = p[1];
    if (flags & 0x3C) return -EINVAL;                               /* reserved bits */
    size_t pos = 2, end = h->size - 4, a;
    h->has_c = flags & 0x40;
    h->has_u = flags & 0x80;
    if (h->has_c) {
        a = vli(p + pos, end - pos, &h->csize);
        if (!a || !h->csize) return -EINVAL;
        pos += a;
    }
    if (h->has_u) {
        a = vli(p + pos, end - pos, &h->usize);
        if (!a) return -EINVAL;
        pos += a;
    }
    h->dict = 0;
    int r = filter_chain(p, end, (flags & 3) + 1, h, pos);
    if (r) return r;
    return h->dict > XZ_DICT_MAX ? -EOPNOTSUPP : 0;
}

typedef struct Dec {
    Lzma *z;
    XzOut out;
    Check chk;
    uint64_t *recs;                    /* unpadded and uncompressed size of every block of the stream */
    size_t nrecs, cap_recs;
} Dec;

static int add_record(Dec *d, uint64_t unpadded, uint64_t usize)
{
    if (d->nrecs == d->cap_recs) {
        size_t cap = d->cap_recs ? d->cap_recs * 2 : 16;
        uint64_t *n = mem_realloc(d->recs, cap * 2 * sizeof *n);
        if (!n) return -ENOMEM;
        d->recs = n;
        d->cap_recs = cap;
    }
    d->recs[2 * d->nrecs] = unpadded;
    d->recs[2 * d->nrecs + 1] = usize;
    d->nrecs++;
    return 0;
}

/* one block at p[0, n): its length with the padding and the check, or an error */
static int64_t block(Dec *d, uint32_t check_id, const uint8_t *p, size_t n)
{
    BlockHdr h;
    int r = block_header(p, n, &h);
    if (r) return r;
    check_begin(&d->chk, check_id == CHECK_CRC32 || check_id == CHECK_CRC64 || check_id == CHECK_SHA256 ? check_id : CHECK_NONE);
    d->out.block_out = 0;
    d->out.check = check_feed;
    d->out.check_ctx = &d->chk;
    size_t used;
    r = lzma2_decode(d->z, &d->out, h.dict, p + h.size, n - h.size, &used);
    if (r) return r;
    if ((h.has_c && h.csize != used) || (h.has_u && h.usize != d->out.block_out)) return -EINVAL;
    size_t pad = (4 - used % 4) % 4, cs = check_size(check_id), total = h.size + used + pad + cs;
    if (n - h.size - used < pad + cs) return -EINVAL;
    for (size_t k = 0; k < pad; k++)
        if (p[h.size + used + k]) return -EINVAL;
    if (!check_ok(&d->chk, p + h.size + used + pad)) return -EBADMSG;
    r = add_record(d, h.size + used + cs, d->out.block_out);
    return r ? r : (int64_t)total;
}

/* the index at p[0, n) (its indicator byte first): its length, verified against the blocks decoded */
static int64_t index_field(Dec *d, const uint8_t *p, size_t n)
{
    uint64_t count;
    size_t pos = 1, a = n > 1 ? vli(p + pos, n - pos, &count) : 0;
    if (!a || count != d->nrecs) return -EINVAL;
    pos += a;
    for (size_t k = 0; k < d->nrecs; k++) {
        uint64_t up, us;
        a = vli(p + pos, n - pos, &up);
        if (!a || up != d->recs[2 * k]) return -EINVAL;
        pos += a;
        a = vli(p + pos, n - pos, &us);
        if (!a || us != d->recs[2 * k + 1]) return -EINVAL;
        pos += a;
    }
    size_t padded = (pos + 3) & ~(size_t)3;
    if (n < padded + 4) return -EINVAL;
    for (size_t k = pos; k < padded; k++)
        if (p[k]) return -EINVAL;
    if (load_le32(p + padded) != ~crc32_update(0xFFFFFFFFu, p, padded)) return -EINVAL;
    return (int64_t)(padded + 4);
}

/* one stream at p[0, n): its length, or an error */
static int64_t stream(Dec *d, const uint8_t *p, size_t n)
{
    static const uint8_t magic[6] = {0xFD, '7', 'z', 'X', 'Z', 0};
    if (n < 12 || __builtin_memcmp(p, magic, 6)) return -EINVAL;
    if (p[6] || (p[7] & 0xF0)) return -EINVAL;                      /* reserved bits of the stream flags */
    if (load_le32(p + 8) != ~crc32_update(0xFFFFFFFFu, p + 6, 2)) return -EINVAL;
    uint32_t check_id = p[7] & 15;
    size_t pos = 12;
    d->nrecs = 0;
    while (pos < n && p[pos]) {
        int64_t b = block(d, check_id, p + pos, n - pos);
        if (b < 0) return b;
        pos += (size_t)b;
    }
    if (pos >= n) return -EINVAL;
    int64_t ix = index_field(d, p + pos, n - pos);
    if (ix < 0) return ix;
    pos += (size_t)ix;
    if (n - pos < 12 || __builtin_memcmp(p + pos + 10, "YZ", 2)) return -EINVAL;
    const uint8_t *f = p + pos;
    if (load_le32(f) != ~crc32_update(0xFFFFFFFFu, f + 4, 6) || ((uint64_t)load_le32(f + 4) + 1) * 4 != (uint64_t)ix) return -EINVAL;
    if (f[8] != p[6] || f[9] != p[7]) return -EINVAL;               /* the footer repeats the stream flags */
    return (int64_t)(pos + 12);
}

intptr_t xz_decode(const void *src, uint64_t src_len, uint64_t max_out, const CodecSink *sink, CodecResult *res)
{
    res->consumed = res->produced = 0;
    Dec *d = mem_alloc(sizeof *d);
    Lzma *z = mem_alloc(sizeof *z);
    if (!d || !z) {
        mem_free(d);
        mem_free(z);
        return -ENOMEM;
    }
    d->z = z;
    d->out.sink = sink;
    d->out.max_out = max_out;
    const uint8_t *p = src;
    uint64_t pos = 0;
    intptr_t rc = src_len ? 0 : -EINVAL;
    for (uint32_t streams = 0; !rc && pos < src_len; streams++) {
        uint64_t n = src_len - pos, pad = 0;
        while (streams && pad < n && !p[pos + pad]) pad++;           /* zero padding between streams: a multiple of four bytes */
        if (pad % 4) {
            rc = -EINVAL;
            break;
        }
        if (pad == n) {                                              /* padding up to the end of the input */
            pos = res->consumed = src_len;
            break;
        }
        int64_t s = stream(d, p + pos + pad, (size_t)(n - pad));
        if (s < 0) rc = s;
        else pos += pad + (uint64_t)s;
        res->consumed = rc ? res->consumed : pos;
    }
    res->produced = d->out.produced;
    mem_free(d->recs);
    mem_free(z);
    mem_free(d);
    return rc;
}
