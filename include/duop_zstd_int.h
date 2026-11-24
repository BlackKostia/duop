/* include/duop_zstd_int.h - what the files of the Zstandard decoder share (src/fs/zstd_dec.c, zstd_fse.c, zstd_huf.c, zstd_seq.c).
 *
 * The bit reader here reads a stream the way Zstandard writes it: backwards, from the last byte, whose highest set bit is an end mark.
 * Bits come out most significant first. Reading past the front yields zero bits and is noticed at the end (rev_finished) or by
 * rev_reload (REV_OVER), so a damaged stream is -EINVAL and never a read outside the buffer. */
#pragma once
#include "duop_abi.h"
#include "duop_hash.h"
#include "duop_pal.h"
#include "duop_zstd.h"

/* ZSTD_STRICT: reject what the format forbids but libzstd lets through (reserved bits of the sequence modes byte, a repeat offset that works out
 * as zero - libzstd makes it 1 -, bits left over at the front of a Huffman stream - its fast path never looks). Real encoders never produce either. The test build of tests/test_c_zstd_dec.py defines
 * ZSTD_LAX to compare with `zstd -d` bit for bit; the product is always strict. */
#ifdef ZSTD_LAX
#define ZSTD_STRICT 0
#else
#define ZSTD_STRICT 1
#endif

#define ZSTD_BLOCK_MAX (128u << 10)
#define ZSTD_FSE_LOG 9                 /* the largest accuracy log of a sequence table */
#define ZSTD_HUF_LOG 12                /* the largest Huffman code length (the format says 11; libzstd reads 12) */

typedef struct FseCell { uint16_t base; uint8_t nbits, sym; } FseCell;
typedef struct FseTable { FseCell cell[1u << ZSTD_FSE_LOG]; uint32_t log; } FseTable;
typedef struct HufCell { uint8_t sym, nbits; } HufCell;
typedef struct HufTable { HufCell cell[1u << ZSTD_HUF_LOG]; uint32_t log; } HufTable;

typedef struct ZstdCtx {
    FseTable ll, of, ml;               /* the tables of the last block that set them (Repeat_Mode uses them again) */
    HufTable huf;                      /* the Huffman table of the last compressed literals (Treeless_Literals_Block) */
    uint32_t have;                     /* ZSTD_HAVE_* */
    uint32_t rep[3];                   /* the repeat offsets */
    uint8_t *win;                      /* history and output: [0, cap) */
    size_t cap, pos, done;             /* pos: next byte to write; [done, pos) is not delivered yet */
    size_t limit;                      /* the current block writes below this */
    uint8_t *lit;                      /* literals of the block (ZSTD_BLOCK_MAX + 32) */
    size_t lit_len;
    uint64_t window;                   /* Window_Size of the frame */
    uint64_t total;                    /* bytes of this frame produced so far */
} ZstdCtx;
#define ZSTD_HAVE_LL 1u
#define ZSTD_HAVE_OF 2u
#define ZSTD_HAVE_ML 4u
#define ZSTD_HAVE_HUF 8u

typedef struct Rev { const uint8_t *start, *ptr; uint64_t cont; uint32_t used; } Rev;   /* used: bits taken from the top of cont */
enum { REV_OK, REV_END, REV_DONE, REV_OVER };       /* reload: more to come / at the front / all consumed exactly / read past the front */

static inline uint32_t zstd_hbit(uint32_t v) { return 31u - (uint32_t)__builtin_clz(v); }   /* v != 0 */

static inline int rev_init(Rev *b, const uint8_t *p, size_t n)
{
    if (!n || !p[n - 1]) return -EINVAL;
    uint32_t hb = zstd_hbit(p[n - 1]);
    b->start = p;
    if (n >= 8) {
        b->ptr = p + n - 8;
        b->cont = load_le64(b->ptr);
        b->used = 8 - hb;
    } else {
        uint8_t tmp[8] = {0};
        __builtin_memcpy(tmp, p, n);
        b->ptr = p;
        b->cont = load_le64(tmp);
        b->used = (8 - (uint32_t)n) * 8 + 8 - hb;
    }
    return 0;
}

static inline uint64_t rev_look(const Rev *b, uint32_t nb) { return nb ? (b->cont << (b->used & 63)) >> (64 - nb) : 0; }   /* nb <= 56 */
static inline uint64_t rev_read(Rev *b, uint32_t nb)
{
    uint64_t v = rev_look(b, nb);
    b->used += nb;
    return v;
}

static inline int rev_reload(Rev *b)
{
    if (b->used > 64) return REV_OVER;
    if (b->ptr == b->start) return b->used == 64 ? REV_DONE : REV_END;
    uint32_t nb = b->used >> 3;
    int st = REV_OK;
    if ((size_t)(b->ptr - b->start) < nb) {
        nb = (uint32_t)(b->ptr - b->start);
        st = REV_END;
    }
    b->ptr -= nb;
    b->used -= nb * 8;
    b->cont = load_le64(b->ptr);
    return st;
}

static inline bool rev_finished(const Rev *b) { return b->ptr == b->start && b->used == 64; }   /* every bit was used, none missing */

/* zstd_fse.c */
/* Reads a table description (normalized counts) from src[0, n) and builds the decoding table: 0, -EINVAL; *used = bytes of the description. */
int zstd_fse_read(FseTable *t, const uint8_t *src, size_t n, uint32_t max_log, uint32_t max_sym, size_t *used);
void zstd_fse_rle(FseTable *t, uint8_t sym);                    /* a table of one symbol that costs no bits */
int zstd_fse_predef(FseTable *t, int which);                    /* 0 literal lengths, 1 offsets, 2 match lengths: the default distributions */
/* zstd_huf.c */
int zstd_huf_table(HufTable *t, const uint8_t *src, size_t n, size_t *used);           /* the tree description at the start of compressed literals */
/* dst_len symbols from one stream (four = 0) or from the four streams behind a 6-byte jump table (four = 1) */
int zstd_huf_decode(const HufTable *t, uint8_t *dst, size_t dst_len, const uint8_t *src, size_t n, bool four);
/* zstd_seq.c */
int zstd_sequences(ZstdCtx *c, const uint8_t *src, size_t n);   /* the sequences section of a compressed block, executed into c->win at c->pos */
