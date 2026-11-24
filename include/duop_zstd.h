/* include/duop_zstd.h - the Zstandard decoder (RFC 8878) of the archive layer (src/fs/zstd_dec.c, zstd_fse.c, zstd_huf.c, zstd_seq.c;
 * docs/ARCHIVE_ROADMAP_2026-11-24.md, phase 4). Contract of include/duop_codec.h: the whole input is in memory, the output leaves through
 * a sink in pieces of at most CODEC_CHUNK bytes, max_out is exact (-EFBIG after delivering exactly max_out bytes).
 *
 * Every frame is decoded in turn (several concatenated frames and skippable frames are one stream, as `zstd -d` does); a byte that
 * does not start a frame is -EINVAL. Frame content size and checksum are verified (-EINVAL / -EBADMSG). A frame that needs a
 * dictionary, or a window beyond 2^27 bytes (the `zstd --long=27` limit), is -EOPNOTSUPP: its data may be fine, it is not decoded.
 * RAM: the history window of the frame (at most twice its size) plus ~160 KiB of tables, never the size of the data. */
#pragma once
#include "duop_codec.h"

#define ZSTD_MAGIC 0xFD2FB528u
#define ZSTD_WINDOW_LOG_LIMIT 27

intptr_t zstd_decode(const void *src, uint64_t src_len, uint64_t max_out, const CodecSink *sink, CodecResult *res);
