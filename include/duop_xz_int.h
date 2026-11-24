/* include/duop_xz_int.h - between src/fs/xz_dec.c and src/fs/lzma2_dec.c: where decoded bytes go, and the LZMA2 chunk decoder. */
#pragma once
#include "duop_lzma_int.h"

typedef struct XzOut {
    const CodecSink *sink;
    uint64_t max_out, produced;            /* the cap over all streams, and what the sink has received */
    uint64_t block_out;                    /* bytes of the current block */
    void (*check)(void *ctx, const uint8_t *p, size_t n);   /* the integrity check of the block, fed with every byte delivered */
    void *check_ctx;
} XzOut;

int xz_out_write(XzOut *o, const uint8_t *p, size_t n);     /* to the check and the sink, in pieces of CODEC_CHUNK, never beyond max_out (-EFBIG) */
/* The LZMA2 data of one block: chunks up to the end mark. A ring of dict_size bytes is allocated for it. 0, -EINVAL, -EFBIG, -ENOMEM, a sink error;
 * *used = bytes taken from src, the end mark included. */
int lzma2_decode(Lzma *z, XzOut *o, uint32_t dict_size, const uint8_t *src, size_t n, size_t *used);
