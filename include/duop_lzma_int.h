/* include/duop_lzma_int.h - what the files of the LZMA / LZMA2 decoder share (src/fs/lzma_dec.c, lzma2_dec.c, xz_dec.c).
 *
 * The dictionary is a ring of `size` bytes that is also the output buffer: lzma_run produces bytes up to a limit, the caller delivers
 * [done, pos) to the sink and wraps pos to 0 when it reaches size. A match that does not fit before the limit continues in the next
 * call (pending). The range decoder never reads outside [in, end): a stream that wants more sets `bad`. */
#pragma once
#include "duop_abi.h"
#include "duop_pal.h"
#include "duop_xz.h"

typedef struct LzmaLen { uint16_t choice, choice2, low[16][8], mid[16][8], high[256]; } LzmaLen;

typedef struct Lzma {
    /* probabilities */
    uint16_t is_match[12][16], is_rep[12], is_rep0[12], is_rep1[12], is_rep2[12], is_rep0_long[12][16];
    uint16_t dist_slot[4][64], dist_special[114], dist_align[16];
    LzmaLen mlen, rlen;
    uint16_t lit[0x300u << 4];                 /* lc + lp <= 4 */
    uint32_t state, rep0, rep1, rep2, rep3;
    uint32_t lc, lp, pb;
    /* range decoder */
    uint32_t range, code;
    const uint8_t *in, *end;
    bool bad;                                  /* the input ran out */
    /* dictionary / output ring */
    uint8_t *buf;
    size_t size, pos, done;
    size_t full;                               /* valid history: at most size */
    uint32_t pending;                          /* bytes of a match still to copy */
} Lzma;

bool lzma_set_props(Lzma *z, uint32_t props);         /* lc / lp / pb from the props byte: false when invalid for LZMA2 (lc + lp > 4) */
void lzma_reset_state(Lzma *z);                       /* probabilities, state and repeat offsets back to their start values */
int lzma_rc_init(Lzma *z, const uint8_t *in, size_t n);   /* starts the range decoder on a chunk's bytes: 0, -EINVAL */
bool lzma_rc_finished(const Lzma *z);                 /* the chunk's bytes are used up exactly and the code is 0 */
/* decodes symbols until pos reaches `limit` (<= size) or the chunk's bytes end: 0, -EINVAL (bad data, end marker, distance beyond the history) */
int lzma_run(Lzma *z, size_t limit);
