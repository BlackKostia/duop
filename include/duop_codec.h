/* include/duop_codec.h - the contract of the archive decoders (src/fs/inflate_stream.c, gzip_dec.c, bzip2_dec.c, xz_dec.c,
 * zstd_dec.c): the whole compressed input is in memory (an mmap of the archive), the output is pushed in chunks through a sink, so
 * the decoders need no refill logic and the RAM they hold is a window, never the unpacked data. See docs/ARCHIVE_TOOLS_PLAN.md.
 *
 * Results are 0 or -errno:
 *   -EINVAL     malformed or truncated stream     -EBADMSG  a stored checksum does not match the data
 *   -EFBIG      output would exceed max_out       -EOPNOTSUPP  a valid stream using a feature that is not implemented
 *   -ENOMEM     work memory unavailable           anything negative a sink returns (-ECANCELED from Esc, -ENOSPC, ...) is passed on */
#pragma once
#include "duop_abi.h"

#define CODEC_UNLIMITED UINT64_MAX      /* max_out: no cap (0 means "the output must be empty") */
#define CODEC_CHUNK 65536u              /* the size of every write() but the last */

typedef struct CodecSink {
    intptr_t (*write)(void *ctx, const uint8_t *buf, size_t len);   /* 0, or a negative error that stops the decoder */
    void *ctx;
} CodecSink;

typedef struct CodecResult {
    uint64_t consumed;                  /* bytes of input the stream occupied (valid on success) */
    uint64_t produced;                  /* bytes handed to the sink (on an error: what was delivered before it) */
} CodecResult;
