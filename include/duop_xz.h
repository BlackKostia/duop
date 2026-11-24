/* include/duop_xz.h - the .xz container with LZMA2 (src/fs/xz_dec.c, lzma2_dec.c, lzma_dec.c; docs/ARCHIVE_ROADMAP_2026-11-24.md, phase 4).
 * Contract of include/duop_codec.h: the whole input is in memory, the output leaves through a sink in pieces of at most CODEC_CHUNK bytes,
 * max_out is exact (-EFBIG after delivering exactly max_out bytes).
 *
 * Every stream is decoded in turn (concatenated streams and zero stream padding are one input, as `xz -d` does); anything else behind the last
 * stream is -EINVAL. Block and stream headers, the stored sizes, the check of every block (none, CRC-32, CRC-64, SHA-256) and the index and
 * footer of every stream are verified (-EINVAL / -EBADMSG). A block with a filter other than LZMA2 (BCJ, Delta) is -EOPNOTSUPP, and so is a
 * dictionary above XZ_DICT_MAX. RAM: the dictionary of the block (a ring of its size, touched only as far as it is used) plus ~30 KiB. */
#pragma once
#include "duop_codec.h"

#define XZ_DICT_MAX (256u << 20)

intptr_t xz_decode(const void *src, uint64_t src_len, uint64_t max_out, const CodecSink *sink, CodecResult *res);
