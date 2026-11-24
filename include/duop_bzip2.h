/* include/duop_bzip2.h - the bzip2 decoder (src/fs/bzip2_dec.c; docs/ARCHIVE_ROADMAP_2026-11-24.md, phase 4).
 * Contract of include/duop_codec.h: the whole input is in memory, the output leaves through a sink in pieces of at most CODEC_CHUNK bytes,
 * max_out is exact (-EFBIG after delivering exactly max_out bytes).
 *
 * Every stream is decoded in turn (concatenated streams are one input, as `bzip2 -d` does); anything else behind the last stream is -EINVAL.
 * The CRC-32 of every block and the combined CRC of every stream are verified (-EBADMSG). A block with the obsolete "randomised" flag is
 * -EOPNOTSUPP. RAM: the block table (4 bytes per byte of the block size, 3.6 MB at level 9) plus ~20 KiB. */
#pragma once
#include "duop_codec.h"

intptr_t bzip2_decode(const void *src, uint64_t src_len, uint64_t max_out, const CodecSink *sink, CodecResult *res);
