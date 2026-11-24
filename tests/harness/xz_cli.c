/* Freestanding harness for src/fs/xz_dec.c: the record protocol of inflate_stream_cli.c with xz_decode as the decoder (flags unused). */
#include "duop_xz.h"
#define CLI_DECODE(stream, len, flags, max_out, sink, res) ((void)(flags), xz_decode(stream, len, max_out, sink, res))
#include "inflate_stream_cli.c"
