/* Freestanding harness for src/fs/bzip2_dec.c: the record protocol of inflate_stream_cli.c with bzip2_decode as the decoder (flags unused). */
#include "duop_bzip2.h"
#define CLI_DECODE(stream, len, flags, max_out, sink, res) ((void)(flags), bzip2_decode(stream, len, max_out, sink, res))
#include "inflate_stream_cli.c"
