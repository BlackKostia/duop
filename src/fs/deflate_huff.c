/* src/fs/deflate_huff.c - the blocks of the DEFLATE compressor (freestanding C23): symbol statistics, length-limited Huffman codes, the
 * dynamic block header, and the choice between stored, fixed and dynamic coding by the exact size of each (as zlib decides).
 *
 * Code lengths come from the usual two-queue Huffman construction; lengths above the limit (15, and 7 for the header's own code) are
 * repaired the way zlib repairs them: the surplus is moved to shorter codes level by level and the lengths are dealt out again, the
 * least frequent symbols getting the longest codes. The bit writer is LSB first into a 64 KiB buffer that goes to the sink when full. */
#include "duop_deflate_int.h"

static const uint16_t len_base[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
static const uint8_t len_extra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static const uint16_t dist_base[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
static const uint8_t dist_extra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
static const uint8_t cl_order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};

void deflate_tables_init(Deflate *d)
{
    for (uint32_t c = 0; c < 28; c++)
        for (uint32_t l = len_base[c]; l < len_base[c] + (1u << len_extra[c]); l++) d->length_code[l - 3] = (uint8_t)c;
    d->length_code[255] = 28;                                       /* length 258 has a code of its own */
    for (uint32_t c = 0; c < 30; c++) {
        for (uint32_t dm = dist_base[c] - 1; dm < dist_base[c] - 1 + (1u << dist_extra[c]); dm++) {
            if (dm < 256) d->dist_code[dm] = (uint8_t)c;
            else d->dist_code[256 + (dm >> 7)] = (uint8_t)c;
        }
    }
}

static inline uint32_t dcode(const Deflate *d, uint32_t dist)
{
    uint32_t dm = dist - 1;
    return dm < 256 ? d->dist_code[dm] : d->dist_code[256 + (dm >> 7)];
}

/* ---- bits ---- */
static int out_flush(Deflate *d)
{
    if (!d->outn) return 0;
    intptr_t r = d->sink->write(d->sink->ctx, d->out, d->outn);
    d->outn = 0;
    return r < 0 ? (int)r : 0;
}

static int put_bits(Deflate *d, uint32_t v, uint32_t n)
{
    d->bitbuf |= (uint64_t)v << d->bitcnt;
    d->bitcnt += n;
    while (d->bitcnt >= 8) {
        d->out[d->outn++] = (uint8_t)d->bitbuf;
        d->bitbuf >>= 8;
        d->bitcnt -= 8;
    }
    return d->outn >= DEF_OUT ? out_flush(d) : 0;
}

int deflate_flush_bits(Deflate *d)
{
    if (d->bitcnt) {
        d->out[d->outn++] = (uint8_t)d->bitbuf;
        d->bitbuf = 0;
        d->bitcnt = 0;
    }
    return out_flush(d);
}

int deflate_emit_stored(Deflate *d, const uint8_t *buf, size_t len, bool last)
{
    int r = put_bits(d, last ? 1 : 0, 3);                             /* BFINAL, BTYPE 00 */
    if (!r && d->bitcnt) r = put_bits(d, 0, 8 - d->bitcnt);
    if (!r) r = put_bits(d, (uint32_t)len, 16);
    if (!r) r = put_bits(d, ~(uint32_t)len & 0xFFFF, 16);
    for (size_t i = 0; !r && i < len;) {
        size_t k = len - i < DEF_OUT - d->outn ? len - i : DEF_OUT - d->outn;
        __builtin_memcpy(d->out + d->outn, buf + i, k);
        d->outn += k;
        i += k;
        if (d->outn >= DEF_OUT) r = out_flush(d);
    }
    return r;
}

/* ---- Huffman codes ---- */
static void sort_symbols(uint16_t *sym, uint32_t n, const uint32_t *freq)
{
    for (uint32_t i = 1; i < n; i++) {                                 /* insertion sort: by frequency, then by symbol */
        uint16_t s = sym[i];
        uint32_t j = i;
        while (j && (freq[sym[j - 1]] > freq[s] || (freq[sym[j - 1]] == freq[s] && sym[j - 1] > s))) {
            sym[j] = sym[j - 1];
            j--;
        }
        sym[j] = s;
    }
}

/* the depth of every leaf of the Huffman tree of the sorted symbols (two-queue construction): depth[i] for sym[i] */
__attribute__((noinline)) static void leaf_depths(const uint16_t *sym, uint32_t n, const uint32_t *freq, uint32_t *depth)
{
    uint32_t w[2 * 286];
    uint16_t parent[2 * 286];
    for (uint32_t i = 0; i < n; i++) w[i] = freq[sym[i]];
    uint32_t leaf = 0, node = n, next = n;                              /* next leaf, next internal node to consume, next internal node to create */
    for (uint32_t made = 0; made + 1 < n; made++) {
        uint32_t pick[2];
        for (int k = 0; k < 2; k++) {
            if (leaf < n && (node >= next || w[leaf] <= w[node])) pick[k] = leaf++;
            else pick[k] = node++;
        }
        w[next] = w[pick[0]] + w[pick[1]];
        parent[pick[0]] = parent[pick[1]] = (uint16_t)next;
        next++;
    }
    uint32_t *dep = w;                                                  /* the weights are no longer needed: the depths take their place */
    dep[next - 1] = 0;
    for (uint32_t k = next - 1; k-- > 0;) dep[k] = dep[parent[k]] + 1;
    for (uint32_t i = 0; i < n; i++) depth[i] = dep[i];
}

/* code lengths (at most max_bits) of n symbols from their frequencies; at least two codes are always made, as decoders expect */
static void build_lengths(uint32_t *freq, uint32_t n, uint8_t *len, uint32_t max_bits)
{
    uint16_t sym[286];
    uint32_t cnt = 0;
    for (uint32_t s = 0; s < n; s++) {
        len[s] = 0;
        if (freq[s]) sym[cnt++] = (uint16_t)s;
    }
    for (uint32_t s = 0; cnt < 2 && s < n; s++) {                      /* a lone symbol (or none) gets a partner with a count of one */
        if (!freq[s]) {
            freq[s] = 1;
            sym[cnt++] = (uint16_t)s;
        }
    }
    sort_symbols(sym, cnt, freq);
    uint32_t depth[286], bl_count[16] = {0}, overflow = 0;
    leaf_depths(sym, cnt, freq, depth);
    for (uint32_t i = 0; i < cnt; i++) {
        if (depth[i] > max_bits) {
            depth[i] = max_bits;
            overflow++;
        }
        bl_count[depth[i]]++;
    }
    while (overflow > 0) {                                              /* move the surplus to shorter codes */
        uint32_t bits = max_bits - 1;
        while (!bl_count[bits]) bits--;
        bl_count[bits]--;
        bl_count[bits + 1] += 2;
        bl_count[max_bits]--;
        overflow = overflow > 2 ? overflow - 2 : 0;
    }
    uint32_t idx = 0;
    for (uint32_t bits = max_bits; bits; bits--)                        /* the longest codes go to the least frequent symbols */
        for (uint32_t k = bl_count[bits]; k; k--) len[sym[idx++]] = (uint8_t)bits;
}

static uint32_t reverse(uint32_t code, uint32_t n)
{
    uint32_t r = 0;
    for (; n; n--, code >>= 1) r = r << 1 | (code & 1);
    return r;
}

/* the canonical codes for the lengths, already bit-reversed for the LSB-first writer */
static void make_codes(const uint8_t *len, uint32_t n, uint16_t *code)
{
    uint32_t count[16] = {0}, next[16];
    for (uint32_t i = 0; i < n; i++) count[len[i]]++;
    count[0] = 0;
    uint32_t c = 0;
    for (uint32_t b = 1; b < 16; b++) {
        c = (c + count[b - 1]) << 1;
        next[b] = c;
    }
    for (uint32_t i = 0; i < n; i++)
        if (len[i]) code[i] = (uint16_t)reverse(next[len[i]]++, len[i]);
}

/* ---- the block ---- */
typedef struct Block {
    uint32_t ll_freq[DEF_LITLEN_CODES], d_freq[DEF_DIST_CODES], cl_freq[19];
    uint8_t ll_len[DEF_LITLEN_CODES], d_len[DEF_DIST_CODES], cl_len[19];
    uint16_t ll_code[DEF_LITLEN_CODES], d_code[DEF_DIST_CODES], cl_code[19];
    uint8_t rle[DEF_LITLEN_CODES + DEF_DIST_CODES + 8];          /* the run-length coded code lengths: symbol, then its extra bits value */
    uint8_t rle_extra[DEF_LITLEN_CODES + DEF_DIST_CODES + 8];
    uint32_t nrle, hlit, hdist, hclen;
    uint64_t extra_bits;
} Block;

static void count_symbols(const Deflate *d, Block *b)
{
    for (uint32_t i = 0; i < d->nsym; i++) {
        if (!d->dist[i]) {
            b->ll_freq[d->lc[i]]++;
        } else {
            uint32_t lc = d->length_code[d->lc[i]], dc = dcode(d, d->dist[i]);
            b->ll_freq[257 + lc]++;
            b->d_freq[dc]++;
            b->extra_bits += len_extra[lc] + dist_extra[dc];
        }
    }
    b->ll_freq[256] = 1;
}

/* the lengths of both codes as one list, run-length coded with symbols 16 / 17 / 18 */
static void encode_lengths(Block *b)
{
    uint8_t all[DEF_LITLEN_CODES + DEF_DIST_CODES];
    b->hlit = DEF_LITLEN_CODES;
    while (b->hlit > 257 && !b->ll_len[b->hlit - 1]) b->hlit--;
    b->hdist = DEF_DIST_CODES;
    while (b->hdist > 1 && !b->d_len[b->hdist - 1]) b->hdist--;
    uint32_t n = 0;
    for (uint32_t i = 0; i < b->hlit; i++) all[n++] = b->ll_len[i];
    for (uint32_t i = 0; i < b->hdist; i++) all[n++] = b->d_len[i];
    b->nrle = 0;
    for (uint32_t i = 0; i < 19; i++) b->cl_freq[i] = 0;
    for (uint32_t i = 0; i < n;) {
        uint32_t run = 1;
        while (i + run < n && all[i + run] == all[i]) run++;
        uint8_t sym, extra = 0;
        uint32_t used;
        if (!all[i] && run >= 11) {
            used = run > 138 ? 138 : run;
            sym = 18;
            extra = (uint8_t)(used - 11);
        } else if (!all[i] && run >= 3) {
            used = run > 10 ? 10 : run;
            sym = 17;
            extra = (uint8_t)(used - 3);
        } else if (all[i] && run >= 4) {                                /* a length, then "repeat it 3-6 times" */
            b->rle[b->nrle] = all[i];
            b->rle_extra[b->nrle++] = 0;
            b->cl_freq[all[i]]++;
            used = run - 1 > 6 ? 6 : run - 1;
            sym = 16;
            extra = (uint8_t)(used - 3);
            used++;
        } else {
            sym = all[i];
            used = 1;
        }
        b->rle[b->nrle] = sym;
        b->rle_extra[b->nrle++] = extra;
        b->cl_freq[sym]++;
        i += used;
    }
}

static int emit_rle(Deflate *d, const Block *b)
{
    int r = 0;
    for (uint32_t i = 0; !r && i < b->nrle; i++) {
        uint8_t s = b->rle[i];
        r = put_bits(d, b->cl_code[s], b->cl_len[s]);
        if (!r && s == 16) r = put_bits(d, b->rle_extra[i], 2);
        else if (!r && s == 17) r = put_bits(d, b->rle_extra[i], 3);
        else if (!r && s == 18) r = put_bits(d, b->rle_extra[i], 7);
    }
    return r;
}

/* the symbols, then the end of block, with the given codes */
static int emit_symbols(Deflate *d, const uint16_t *ll_code, const uint8_t *ll_len, const uint16_t *d_code, const uint8_t *d_len)
{
    int r = 0;
    for (uint32_t i = 0; !r && i < d->nsym; i++) {
        if (!d->dist[i]) {
            r = put_bits(d, ll_code[d->lc[i]], ll_len[d->lc[i]]);
            continue;
        }
        uint32_t lc = d->length_code[d->lc[i]], dc = dcode(d, d->dist[i]);
        r = put_bits(d, ll_code[257 + lc], ll_len[257 + lc]);
        if (!r && len_extra[lc]) r = put_bits(d, (uint32_t)(d->lc[i] + 3 - len_base[lc]), len_extra[lc]);
        if (!r) r = put_bits(d, d_code[dc], d_len[dc]);
        if (!r && dist_extra[dc]) r = put_bits(d, d->dist[i] - dist_base[dc], dist_extra[dc]);
    }
    return r ? r : put_bits(d, ll_code[256], ll_len[256]);
}

static uint64_t code_bits(const uint32_t *freq, const uint8_t *len, uint32_t n)
{
    uint64_t t = 0;
    for (uint32_t i = 0; i < n; i++) t += (uint64_t)freq[i] * len[i];
    return t;
}

int deflate_emit_block(Deflate *d, const uint8_t *stored, size_t stored_len, bool last)
{
    Block *b = mem_alloc(sizeof *b);
    if (!b) return -ENOMEM;
    count_symbols(d, b);
    uint32_t llf[DEF_LITLEN_CODES], df[DEF_DIST_CODES];
    __builtin_memcpy(llf, b->ll_freq, sizeof llf);
    __builtin_memcpy(df, b->d_freq, sizeof df);
    build_lengths(llf, DEF_LITLEN_CODES, b->ll_len, 15);
    build_lengths(df, DEF_DIST_CODES, b->d_len, 15);
    encode_lengths(b);
    build_lengths(b->cl_freq, 19, b->cl_len, 7);
    b->hclen = 19;
    while (b->hclen > 4 && !b->cl_len[cl_order[b->hclen - 1]]) b->hclen--;
    uint64_t hdr = 14 + 3ull * b->hclen + code_bits(b->cl_freq, b->cl_len, 19);
    for (uint32_t i = 0; i < b->nrle; i++) hdr += b->rle[i] == 16 ? 2 : b->rle[i] == 17 ? 3 : b->rle[i] == 18 ? 7 : 0;
    uint64_t dyn = 3 + hdr + code_bits(b->ll_freq, b->ll_len, DEF_LITLEN_CODES) + code_bits(b->d_freq, b->d_len, DEF_DIST_CODES) + b->extra_bits;
    uint64_t fixed = 3 + b->extra_bits + 5ull * (b->d_freq[0] + b->d_freq[1] + b->d_freq[2] + b->d_freq[3] + b->d_freq[4] + b->d_freq[5] + b->d_freq[6] + b->d_freq[7]);
    uint64_t fd = 0;
    for (uint32_t i = 8; i < DEF_DIST_CODES; i++) fd += b->d_freq[i];
    fixed += 5 * fd;
    for (uint32_t i = 0; i < 286; i++) fixed += (uint64_t)b->ll_freq[i] * (i < 144 ? 8 : i < 256 ? 9 : i < 280 ? 7 : 8);
    uint64_t dyn_bytes = (dyn + 7) >> 3, fixed_bytes = (fixed + 7) >> 3, opt = fixed_bytes <= dyn_bytes ? fixed_bytes : dyn_bytes;
    int r;
    if (stored && stored_len <= 65535 && stored_len + 4 <= opt) {
        r = deflate_emit_stored(d, stored, stored_len, last);
    } else if (fixed_bytes == opt) {
        uint8_t ll_len[288], d_len[30];
        uint16_t ll_code[288], d_code[30];
        for (uint32_t i = 0; i < 288; i++) ll_len[i] = i < 144 ? 8 : i < 256 ? 9 : i < 280 ? 7 : 8;
        for (uint32_t i = 0; i < 30; i++) d_len[i] = 5;
        make_codes(ll_len, 288, ll_code);
        make_codes(d_len, 30, d_code);
        r = put_bits(d, (last ? 1u : 0u) | 1u << 1, 3);
        if (!r) r = emit_symbols(d, ll_code, ll_len, d_code, d_len);
    } else {
        make_codes(b->ll_len, DEF_LITLEN_CODES, b->ll_code);
        make_codes(b->d_len, DEF_DIST_CODES, b->d_code);
        make_codes(b->cl_len, 19, b->cl_code);
        r = put_bits(d, (last ? 1u : 0u) | 2u << 1, 3);
        if (!r) r = put_bits(d, b->hlit - 257, 5);
        if (!r) r = put_bits(d, b->hdist - 1, 5);
        if (!r) r = put_bits(d, b->hclen - 4, 4);
        for (uint32_t i = 0; !r && i < b->hclen; i++) r = put_bits(d, b->cl_len[cl_order[i]], 3);
        if (!r) r = emit_rle(d, b);
        if (!r) r = emit_symbols(d, b->ll_code, b->ll_len, b->d_code, b->d_len);
    }
    mem_free(b);
    d->nsym = 0;
    return r;
}
