/* include/duop_deflate_int.h - what the files of the DEFLATE compressor share (src/fs/deflate.c: matching, src/fs/deflate_huff.c: blocks and codes). */
#pragma once
#include "duop_deflate.h"
#include "duop_oop.h"
#include "duop_pal.h"

enum {
    DEF_WSIZE = 32768, DEF_WMASK = DEF_WSIZE - 1, DEF_MIN_MATCH = 3, DEF_MAX_MATCH = 258, DEF_MIN_LOOKAHEAD = DEF_MAX_MATCH + DEF_MIN_MATCH + 1,
    DEF_MAX_DIST = DEF_WSIZE - DEF_MIN_LOOKAHEAD, DEF_HASH_BITS = 15, DEF_HASH_SIZE = 1 << DEF_HASH_BITS, DEF_SYMS = 16384, DEF_OUT = 65536,
    DEF_LITLEN_CODES = 286, DEF_DIST_CODES = 30
};

struct Deflate {
    const CodecSink *sink;
    int level, err;
    uint32_t good, lazy, nice, chain;       /* the level's matching effort */
    bool lazy_mode;
    uint8_t *win;                           /* 2 * DEF_WSIZE: history and lookahead (level 0: the stored block being filled) */
    uint16_t *head, *prev;                  /* hash chains over window positions, 0 = none */
    uint32_t strstart, lookahead;
    int64_t block_start;                    /* window offset of the first byte of the current block, negative once it has slid out */
    uint32_t match_start, match_length, prev_length, prev_match;
    bool match_available;
    uint8_t *lc;                            /* per symbol: a literal byte, or match length - 3 */
    uint16_t *dist;                         /* per symbol: 0 for a literal, else the distance */
    uint32_t nsym;
    uint8_t *out;                           /* compressed bytes not yet given to the sink */
    size_t outn;
    uint64_t bitbuf;
    uint32_t bitcnt;
    uint8_t length_code[256], dist_code[512];
};

void deflate_tables_init(Deflate *d);
/* Writes the symbols collected so far as one block (the smallest of stored / fixed / dynamic); stored may be NULL when the block's bytes are no
 * longer in the window. Resets nsym. 0 or the sink's error. */
int deflate_emit_block(Deflate *d, const uint8_t *stored, size_t stored_len, bool last);
int deflate_emit_stored(Deflate *d, const uint8_t *buf, size_t len, bool last);     /* one stored block of at most 65535 bytes */
int deflate_flush_bits(Deflate *d);                                                 /* pads to a byte and hands everything to the sink */
