/* src/fs/bzip2_dec.c - the bzip2 decoder (freestanding C23): Huffman + move-to-front + run-length coded symbols, the inverse
 * Burrows-Wheeler transform, the final run-length stage, and the CRC-32 of every block and stream.
 *
 * The input is the whole file in memory and is read as an MSB-first bit stream; reading past its end yields zero bits and is noticed
 * when a symbol or a block is finished, so truncated input is -EINVAL and never an out-of-range read. A block is decoded into one table
 * of at most 900000 entries (level * 100000 bytes of symbols, 4 bytes each, the successor links in the upper 24 bits), then walked
 * once to produce the bytes. Every size read from the stream is checked against the limits of the format before it is used. */
#include "duop_bzip2.h"
#include "duop_oop.h"
#include "duop_pal.h"

enum { MAX_GROUPS = 6, MAX_ALPHA = 258, MAX_LEN = 20, GROUP_SIZE = 50, MAX_SELECTORS = 18002, RUNA = 0, RUNB = 1, CODE_LEN_SLOTS = 23 };

typedef struct Bits {
    const uint8_t *p, *start, *end;
    uint64_t acc;
    uint32_t n;                        /* bits in acc */
    bool over;                         /* something was read past the end of the input */
} Bits;

static inline uint32_t get(Bits *b, uint32_t k)                /* k <= 24 */
{
    while (b->n < k) {
        uint32_t c = 0;
        if (b->p < b->end) c = *b->p++;
        else b->over = true;
        b->acc = b->acc << 8 | c;
        b->n += 8;
    }
    b->n -= k;
    return (uint32_t)(b->acc >> b->n) & ((1u << k) - 1);
}

typedef struct Bz {
    Bits b;
    const CodecSink *sink;
    uint64_t max_out, produced;
    uint32_t *tt;                      /* the block: symbols below, links above */
    uint32_t crc_tab[256], crc;        /* CRC-32 with the MSB-first polynomial 0x04C11DB7, running value of the block */
    uint32_t unzftab[256], cftab[257];
    uint8_t seq2unseq[256], yy[256], selector[MAX_SELECTORS];
    uint8_t len[MAX_GROUPS][MAX_ALPHA];
    int32_t limit[MAX_GROUPS][CODE_LEN_SLOTS], base[MAX_GROUPS][CODE_LEN_SLOTS], perm[MAX_GROUPS][MAX_ALPHA];
    uint8_t min_len[MAX_GROUPS];
    size_t outn;
    uint8_t out[CODEC_CHUNK];
} Bz;

static void crc_init(Bz *z)
{
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i << 24;
        for (int k = 0; k < 8; k++) c = c & 0x80000000u ? c << 1 ^ 0x04C11DB7u : c << 1;
        z->crc_tab[i] = c;
    }
}

/* delivers what is buffered (and hashes it for the block CRC), never beyond max_out */
static int flush(Bz *z)
{
    size_t n = z->outn;
    for (size_t i = 0; i < n; i++) z->crc = z->crc << 8 ^ z->crc_tab[(z->crc >> 24) ^ z->out[i]];
    z->outn = 0;
    uint64_t room = z->max_out - z->produced;
    bool over = n > room;
    if (over) n = (size_t)room;
    if (n) {
        intptr_t r = z->sink->write(z->sink->ctx, z->out, n);
        if (r < 0) return (int)r;
        z->produced += n;
    }
    return over ? -EFBIG : 0;
}

static inline int put(Bz *z, uint8_t c, uint32_t count)
{
    while (count) {
        size_t room = CODEC_CHUNK - z->outn, k = count < room ? count : (uint32_t)room;
        __builtin_memset(z->out + z->outn, c, k);
        z->outn += k;
        count -= (uint32_t)k;
        if (z->outn == CODEC_CHUNK) {
            int r = flush(z);
            if (r) return r;
        }
    }
    return 0;
}

/* the decoding tables of one group from its code lengths (libbz2's hbCreateDecodeTables) */
static void make_tables(Bz *z, uint32_t t, uint32_t alpha)
{
    uint32_t mn = 32, mx = 0;
    for (uint32_t i = 0; i < alpha; i++) {
        if (z->len[t][i] > mx) mx = z->len[t][i];
        if (z->len[t][i] < mn) mn = z->len[t][i];
    }
    int32_t *base = z->base[t], *limit = z->limit[t], *perm = z->perm[t];
    uint32_t pp = 0;
    for (uint32_t i = mn; i <= mx; i++)
        for (uint32_t j = 0; j < alpha; j++)
            if (z->len[t][j] == i) perm[pp++] = (int32_t)j;
    for (int i = 0; i < CODE_LEN_SLOTS; i++) base[i] = limit[i] = 0;
    for (uint32_t i = 0; i < alpha; i++) base[z->len[t][i] + 1]++;
    for (int i = 1; i < CODE_LEN_SLOTS; i++) base[i] += base[i - 1];
    int32_t vec = 0;
    for (uint32_t i = mn; i <= mx; i++) {
        vec += base[i + 1] - base[i];
        limit[i] = vec - 1;
        vec <<= 1;
    }
    for (uint32_t i = mn + 1; i <= mx; i++) base[i] = ((limit[i - 1] + 1) << 1) - base[i];
    z->min_len[t] = (uint8_t)mn;
}

/* the next Huffman symbol of group t, or -1 for a code that is not in the table */
static int next_symbol(Bz *z, uint32_t t)
{
    Bits *b = &z->b;
    uint32_t zn = z->min_len[t];
    int32_t zvec = (int32_t)get(b, zn);
    for (;;) {
        if (zn > MAX_LEN) return -1;
        if (zvec <= z->limit[t][zn]) break;
        zn++;
        zvec = zvec << 1 | (int32_t)get(b, 1);
    }
    int32_t k = zvec - z->base[t][zn];
    return k < 0 || k >= MAX_ALPHA ? -1 : z->perm[t][k];
}

/* the symbol map, the selectors and the coding tables at the start of a block: alphabet size, groups and selectors, or an error */
static int block_tables(Bz *z, uint32_t *alpha_out, uint32_t *nsel_out)
{
    Bits *b = &z->b;
    uint32_t used16 = get(b, 16), in_use = 0;
    for (uint32_t i = 0; i < 16; i++) {
        if (!(used16 & (0x8000u >> i))) continue;
        uint32_t m = get(b, 16);
        for (uint32_t j = 0; j < 16; j++)
            if (m & (0x8000u >> j)) z->seq2unseq[in_use++] = (uint8_t)(i * 16 + j);
    }
    if (!in_use) return -EINVAL;
    uint32_t alpha = in_use + 2, groups = get(b, 3), nsel = get(b, 15);
    if (groups < 2 || groups > MAX_GROUPS || nsel < 1) return -EINVAL;
    uint8_t pos[MAX_GROUPS];
    for (uint32_t i = 0; i < groups; i++) pos[i] = (uint8_t)i;
    for (uint32_t i = 0; i < nsel; i++) {                          /* selectors: unary, then move-to-front; extras beyond the limit are read and dropped */
        uint32_t j = 0;
        while (get(b, 1)) {
            if (++j >= groups) return -EINVAL;
        }
        uint8_t v = pos[j];
        for (; j > 0; j--) pos[j] = pos[j - 1];
        pos[0] = v;
        if (i < MAX_SELECTORS) z->selector[i] = v;
    }
    for (uint32_t t = 0; t < groups; t++) {                         /* code lengths, delta coded */
        int32_t cur = (int32_t)get(b, 5);
        for (uint32_t i = 0; i < alpha; i++) {
            for (;;) {
                if (cur < 1 || cur > MAX_LEN) return -EINVAL;
                if (!get(b, 1)) break;
                cur += get(b, 1) ? -1 : 1;
            }
            z->len[t][i] = (uint8_t)cur;
        }
        make_tables(z, t, alpha);
    }
    *alpha_out = alpha;
    *nsel_out = nsel < MAX_SELECTORS ? nsel : MAX_SELECTORS;
    return b->over ? -EINVAL : 0;
}

/* the Huffman / move-to-front / run-length symbols into tt: the number of entries, or a negative error */
static int64_t read_symbols(Bz *z, uint32_t alpha, uint32_t nsel, uint32_t max_block)
{
    Bits *b = &z->b;
    uint32_t eob = alpha - 1, group = 0, left = 0, nblock = 0;
    for (int i = 0; i < 256; i++) {
        z->yy[i] = (uint8_t)i;
        z->unzftab[i] = 0;
    }
    int sym = 0;
    bool fetch = true;
    for (;;) {
        if (fetch) {
            if (!left) {
                if (group >= nsel) return -EINVAL;
                left = GROUP_SIZE;
                group++;
            }
            left--;
            sym = next_symbol(z, z->selector[group - 1]);
            if (sym < 0 || b->over) return -EINVAL;
        }
        fetch = true;
        if ((uint32_t)sym == eob) break;
        if (sym == RUNA || sym == RUNB) {                           /* a run of the front symbol, its length in bijective base 2 */
            uint32_t es = 0, n = 1;
            while (sym == RUNA || sym == RUNB) {
                if (n >= (2u << 20)) return -EINVAL;
                es += (sym == RUNA ? 1 : 2) * n;
                n <<= 1;
                if (!left) {
                    if (group >= nsel) return -EINVAL;
                    left = GROUP_SIZE;
                    group++;
                }
                left--;
                sym = next_symbol(z, z->selector[group - 1]);
                if (sym < 0 || b->over) return -EINVAL;
            }
            uint8_t uc = z->seq2unseq[z->yy[0]];
            if (es > max_block - nblock) return -EINVAL;
            z->unzftab[uc] += es;
            for (uint32_t i = 0; i < es; i++) z->tt[nblock++] = uc;
            fetch = false;                                          /* sym is the symbol after the run */
            continue;
        }
        if (nblock >= max_block) return -EINVAL;
        uint32_t nn = (uint32_t)sym - 1;                            /* a position in the move-to-front list */
        uint8_t uc = z->yy[nn];
        __builtin_memmove(z->yy + 1, z->yy, nn);
        z->yy[0] = uc;
        uint8_t byte = z->seq2unseq[uc];
        z->unzftab[byte]++;
        z->tt[nblock++] = byte;
    }
    return nblock;
}

/* the inverse transform and the run-length stage of a block of n entries starting at origin: 0 or an error */
static int write_block(Bz *z, uint32_t n, uint32_t origin)
{
    uint32_t sum = 0;
    for (int i = 0; i < 256; i++) {
        z->cftab[i] = sum;
        sum += z->unzftab[i];
    }
    for (uint32_t i = 0; i < n; i++) {
        uint8_t uc = (uint8_t)z->tt[i];
        z->tt[z->cftab[uc]++] |= i << 8;
    }
    uint32_t pos = z->tt[origin] >> 8;
    int prev = -1, run = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t e = z->tt[pos];
        uint8_t ch = (uint8_t)e;
        pos = e >> 8;
        if (run == 4) {                                             /* four equal bytes were followed by a repeat count */
            int r = put(z, (uint8_t)prev, ch);
            if (r) return r;
            run = 0;
            prev = -1;
            continue;
        }
        run = ch == prev ? run + 1 : 1;
        prev = ch;
        int r = put(z, ch, 1);
        if (r) return r;
    }
    return 0;
}

/* one block after its magic: the combined CRC is updated; 0 or an error */
static int block(Bz *z, uint32_t max_block, uint32_t *combined)
{
    Bits *b = &z->b;
    uint32_t stored = get(b, 16) << 16;
    stored |= get(b, 16);
    if (get(b, 1)) return -EOPNOTSUPP;                              /* the obsolete randomised blocks */
    uint32_t origin = get(b, 24), alpha, nsel;
    int r = block_tables(z, &alpha, &nsel);
    if (r) return r;
    int64_t n = read_symbols(z, alpha, nsel, max_block);
    if (n < 0) return (int)n;
    if (origin >= (uint32_t)n) return -EINVAL;
    z->crc = 0xFFFFFFFFu;
    z->outn = 0;
    r = write_block(z, (uint32_t)n, origin);
    if (!r) r = flush(z);
    if (r) return r;
    if (~z->crc != stored) return -EBADMSG;
    *combined = (*combined << 1 | *combined >> 31) ^ stored;
    return b->over ? -EINVAL : 0;
}

/* one stream at the reader's position: 0 or an error */
static int stream(Bz *z)
{
    Bits *b = &z->b;
    if (get(b, 8) != 'B' || get(b, 8) != 'Z' || get(b, 8) != 'h') return -EINVAL;
    uint32_t level = get(b, 8);
    if (level < '1' || level > '9' || b->over) return -EINVAL;
    uint32_t max_block = (level - '0') * 100000u, combined = 0;
    z->tt = mem_alloc((size_t)max_block * sizeof(uint32_t));
    if (!z->tt) return -ENOMEM;
    int r = 0;
    for (;;) {
        uint32_t m1 = get(b, 24), m2 = get(b, 24);
        if (m1 == 0x314159 && m2 == 0x265359) {
            r = block(z, max_block, &combined);
            if (r) break;
        } else if (m1 == 0x177245 && m2 == 0x385090) {
            uint32_t stored = get(b, 16) << 16;
            stored |= get(b, 16);
            r = b->over ? -EINVAL : stored != combined ? -EBADMSG : 0;
            break;
        } else {
            r = -EINVAL;
            break;
        }
    }
    mem_free(z->tt);
    z->tt = nullptr;
    return r;
}

intptr_t bzip2_decode(const void *src, uint64_t src_len, uint64_t max_out, const CodecSink *sink, CodecResult *res)
{
    res->consumed = res->produced = 0;
    Bz *z = mem_alloc(sizeof *z);
    if (!z) return -ENOMEM;
    crc_init(z);
    z->sink = sink;
    z->max_out = max_out;
    z->b.p = z->b.start = src;
    z->b.end = z->b.p + src_len;
    intptr_t rc = src_len ? 0 : -EINVAL;
    while (!rc && z->b.p - z->b.start - (z->b.n >> 3) < (int64_t)src_len) {
        rc = stream(z);
        if (!rc) {
            z->b.n &= ~7u;                                          /* a stream ends on a byte boundary */
            res->consumed = (uint64_t)(z->b.p - z->b.start) - (z->b.n >> 3);
        }
    }
    res->produced = z->produced;
    mem_free(z);
    return rc;
}
