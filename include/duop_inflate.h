/* include/duop_inflate.h - DEFLATE / zlib decompressors (src/fs/inflate.c, src/fs/inflate_stream.c). */
#pragma once
#include "duop_abi.h"
#include "duop_codec.h"

/* Both return the bytes written to dst (at most dst_cap) or -1 for a malformed stream. */
int64_t inflate_raw(const void *src, uint64_t src_len, void *dst, uint64_t dst_cap);
int64_t inflate_zlib(const void *src, uint64_t src_len, void *dst, uint64_t dst_cap);   /* RFC 1950 wrapper */

/* Streaming RFC 1951 (src/fs/inflate_stream.c): src holds the whole stream, the output goes to the sink in CODEC_CHUNK pieces from a
 * 64 KiB window (RAM does not depend on the size of the data). Strict: a truncated stream, an over-subscribed code, a distance before
 * the start or beyond the window are -EINVAL. The result says where the stream ended, so a gzip trailer can be found behind it. */
#define INFLATE_DEFLATE64 1u            /* PKZIP "Deflate64": 64 KiB window, 16 extra bits on length code 285, distance codes 30 / 31 */
intptr_t inflate_stream(const void *src, uint64_t src_len, uint32_t flags, uint64_t max_out, const CodecSink *sink, CodecResult *res);

/* RFC 1952 gzip (src/fs/gzip_dec.c): every member in turn (as gzip -d), each checked against its stored CRC-32 and ISIZE (-EBADMSG) and
 * a header CRC if present; max_out counts all members together. Bytes behind the last member that do not start a member (zero
 * padding, text) are left alone: res->consumed ends after the last member. A member header with a reserved flag bit is -EINVAL. */
intptr_t gzip_decode(const void *src, uint64_t src_len, uint64_t max_out, const CodecSink *sink, CodecResult *res);
