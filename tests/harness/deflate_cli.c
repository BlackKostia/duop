/* Freestanding harness for src/fs/deflate.c: argv = <level> <write size, 0 = all at once>; stdin = the data, stdout = the raw DEFLATE stream, exit 0
 * (or the negative errno of the compressor as 1..255 magnitude). */
#include "script_cli.h"
#include "duop_deflate.h"
#include "duop_oop.h"
#include "duop_pal.h"
#include "duop_sys.h"

static uint8_t g_in[64u << 20];

static intptr_t out_write(void *ctx, const uint8_t *buf, size_t len)
{
    (void)ctx;
    intptr_t r = sys_write_all(1, buf, len);
    return r < 0 ? r : 0;
}

static uint64_t num(const char *s)
{
    uint64_t v = 0;
    for (; *s >= '0' && *s <= '9'; s++) v = v * 10 + (uint64_t)(*s - '0');
    return v;
}

int duop_main(int argc, char **argv, char **envp, const uintptr_t *auxv)
{
    (void)envp; (void)auxv;
    if (argc < 3) return 2;
    size_t n = 0;
    for (;;) {
        intptr_t r = os_read(0, g_in + n, sizeof g_in - n);
        if (r <= 0) break;
        n += (size_t)r;
    }
    uint64_t chunk = num(argv[2]);
    CodecSink sink = {out_write, nullptr};
    Deflate *d = deflate_new((int)num(argv[1]), &sink);
    if (!d) return 3;
    int rc = 0;
    for (size_t off = 0; !rc && off < n;) {
        size_t k = chunk && chunk < n - off ? (size_t)chunk : n - off;
        rc = deflate_write(d, g_in + off, k);
        off += k;
    }
    if (!rc) rc = deflate_finish(d);
    deflate_free(d);
    return rc ? 4 : 0;
}
