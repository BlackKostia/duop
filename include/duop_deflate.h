/* include/duop_deflate.h - the DEFLATE compressor (src/fs/deflate.c, deflate_huff.c; docs/ARCHIVE_ROADMAP_2026-11-24.md, phase 6).
 *
 * Raw RFC 1951 streams for gzip and zip writers. The output depends only on the input and the level - never on how the input was cut into
 * deflate_write calls - so an archive made twice is the same bytes. Levels: 0 stored; 1-3 greedy matching, short chains; 4-9 lazy matching, longer
 * chains. Every block is the smallest of stored, fixed Huffman and dynamic Huffman. RAM: ~330 KiB (window, hash chains, symbol buffer, output). */
#pragma once
#include "duop_codec.h"

typedef struct Deflate Deflate;

Deflate *deflate_new(int level, const CodecSink *sink);                 /* NULL when out of memory; the sink gets the compressed bytes */
int deflate_write(Deflate *d, const uint8_t *buf, size_t len);          /* 0, or the (negative) error of the sink */
int deflate_finish(Deflate *d);                                         /* the last block; the stream is complete after it */
void deflate_free(Deflate *d);
