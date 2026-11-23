/* Freestanding harness for src/fs/inflate_stream.c. stdin = records
 *   [u32 mode][u32 flags][u64 max_out][u64 fail_after][u32 len][len stream bytes]
 * mode 0: inflate_stream only;  mode 1: also inflate_raw on the same bytes (reference for the differential checks).
 * The sink hashes everything (CRC-32, zlib.crc32 compatible), counts chunks, and fails with -ECANCELED once more than fail_after bytes (0 = never) would be
 * delivered. One line per record:
 *   rc=<r> in=<consumed> out=<produced> h=<crc32> chunks=<n> short=<chunks before the last that were not 65536 bytes> [raw=<n> rh=<crc32>] */
#include "script_cli.h"
#include "duop_codec.h"
#include "duop_hash.h"
#include "duop_inflate.h"
#include "duop_pal.h"

#ifndef CLI_DECODE                                   /* tests/harness/gzip_cli.c includes this file with its own decoder */
#define CLI_DECODE(stream, len, flags, max_out, sink, res) inflate_stream(stream, len, flags, max_out, sink, res)
#endif

static uint8_t g_in[48u << 20], g_ref[24u << 20];

typedef struct {
    uint64_t hash, delivered, fail_after, chunks, shorts;
    bool last_short;
} Probe;

static intptr_t probe_write(void *ctx, const uint8_t *buf, size_t len)
{
    Probe *p = ctx;
    if (p->fail_after && p->delivered + len > p->fail_after) return -ECANCELED;
    if (p->last_short) p->shorts++;                          /* a short chunk followed by another one */
    p->last_short = len != CODEC_CHUNK;
    p->hash = crc32_update((uint32_t)p->hash, buf, len);
    p->delivered += len;
    p->chunks++;
    return 0;
}

int duop_main(int argc, char **argv, char **envp, const uintptr_t *auxv)
{
    (void)argc; (void)argv; (void)envp; (void)auxv;
    size_t n = 0;
    for (;;) {
        intptr_t r = os_read(0, g_in + n, sizeof g_in - n);
        if (r <= 0) break;
        n += (size_t)r;
    }
    for (size_t off = 0; off + 28 <= n;) {
        uint32_t mode = load_le32(g_in + off), flags = load_le32(g_in + off + 4), len = load_le32(g_in + off + 24);
        uint64_t max_out = load_le64(g_in + off + 8), fail_after = load_le64(g_in + off + 16);
        const uint8_t *stream = g_in + off + 28;
        if (len > n - off - 28) return 2;
        Probe p = {0xFFFFFFFFu, 0, fail_after, 0, 0, false};
        CodecSink sink = {probe_write, &p};
        CodecResult res = {0, 0};
        intptr_t rc = CLI_DECODE(stream, len, flags, max_out, &sink, &res);
        sc_put("rc="); sc_num(rc);
        sc_put(" in="); sc_num((int64_t)res.consumed);
        sc_put(" out="); sc_num((int64_t)res.produced);
        sc_put(" h="); sc_num((int64_t)(~(uint32_t)p.hash));
        sc_put(" chunks="); sc_num((int64_t)p.chunks);
        sc_put(" short="); sc_num((int64_t)p.shorts);
        if (mode == 1) {
            int64_t k = inflate_raw(stream, len, g_ref, sizeof g_ref);
            sc_put(" raw="); sc_num(k);
            sc_put(" rh="); sc_num(k >= 0 ? (int64_t)(~crc32_update(0xFFFFFFFFu, g_ref, (size_t)k)) : -1);
        }
        sc_put("\n");
        off += 28 + len;
    }
    sc_flush();
    return 0;
}
