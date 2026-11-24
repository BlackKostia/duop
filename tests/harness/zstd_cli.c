/* Freestanding harness for src/fs/zstd_dec.c: the record protocol of inflate_stream_cli.c with zstd_decode as the decoder (flags unused). */
#include "duop_zstd.h"
#define CLI_DECODE(stream, len, flags, max_out, sink, res) ((void)(flags), zstd_decode(stream, len, max_out, sink, res))
#include "inflate_stream_cli.c"
