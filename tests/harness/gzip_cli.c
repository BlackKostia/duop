/* Freestanding harness for src/fs/gzip_dec.c: the record protocol of inflate_stream_cli.c with gzip_decode as the decoder
 * (flags are unused; mode 1 would compare with inflate_raw, which is meaningless here and not used). */
#include "duop_inflate.h"
#define CLI_DECODE(stream, len, flags, max_out, sink, res) ((void)(flags), gzip_decode(stream, len, max_out, sink, res))
#include "inflate_stream_cli.c"
