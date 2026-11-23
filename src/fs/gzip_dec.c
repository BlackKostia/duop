/* src/fs/gzip_dec.c - RFC 1952 gzip members over inflate_stream (freestanding C23).
 *
 * The CRC-32 and the length of the output are taken on the way to the caller's sink (a tee), so nothing is buffered: a member of any
 * size is verified in the 64 KiB window of the inflater. Concatenated members are decoded in turn; whatever follows the last one and
 * does not start with the gzip magic (the zero padding of some tar writers) is not part of the stream. */
#include "duop_hash.h"
#include "duop_inflate.h"
#include "duop_pal.h"

#define FHCRC 2u
#define FEXTRA 4u
#define FNAME 8u
#define FCOMMENT 16u

typedef struct {
    const CodecSink *down;
    uint32_t crc;                  /* running CRC-32 (raw: seed ~0) */
    uint64_t size;
} GzTee;

static intptr_t tee_write(void *ctx, const uint8_t *buf, size_t len)
{
    GzTee *t = ctx;
    t->crc = crc32_update(t->crc, buf, len);
    t->size += len;
    return t->down->write(t->down->ctx, buf, len);
}

/* the length of the member header at p (len bytes available), or -EINVAL / -EBADMSG */
static int64_t gz_header(const uint8_t *p, uint64_t len)
{
    if (len < 10 || p[0] != 0x1F || p[1] != 0x8B || p[2] != 8) return -EINVAL;
    uint32_t flg = p[3];
    if (flg & 0xE0) return -EINVAL;
    uint64_t pos = 10;
    if (flg & FEXTRA) {
        if (len - pos < 2) return -EINVAL;
        uint64_t xl = load_le16(p + pos);
        pos += 2;
        if (len - pos < xl) return -EINVAL;
        pos += xl;
    }
    for (uint32_t f = FNAME; f <= FCOMMENT; f <<= 1) {                       /* NUL-terminated name, comment */
        if (!(flg & f)) continue;
        while (pos < len && p[pos]) pos++;
        if (pos >= len) return -EINVAL;
        pos++;
    }
    if (flg & FHCRC) {
        if (len - pos < 2) return -EINVAL;
        uint32_t want = load_le16(p + pos), got = ~crc32_update(0xFFFFFFFFu, p, pos) & 0xFFFFu;
        if (want != got) return -EBADMSG;
        pos += 2;
    }
    return (int64_t)pos;
}

intptr_t gzip_decode(const void *src, uint64_t src_len, uint64_t max_out, const CodecSink *sink, CodecResult *res)
{
    const uint8_t *p = src;
    uint64_t pos = 0, produced = 0;
    res->consumed = res->produced = 0;
    for (uint32_t members = 0;; members++) {
        if (members && (src_len - pos < 2 || p[pos] != 0x1F || p[pos + 1] != 0x8B)) break;   /* not another member */
        int64_t h = gz_header(p + pos, src_len - pos);
        if (h < 0) return h;
        GzTee tee = {sink, 0xFFFFFFFFu, 0};
        CodecSink s = {tee_write, &tee};
        CodecResult r;
        intptr_t rc = inflate_stream(p + pos + h, src_len - pos - (uint64_t)h, 0, max_out - produced, &s, &r);
        produced += r.produced;
        res->produced = produced;
        if (rc) return rc;
        uint64_t tpos = pos + (uint64_t)h + r.consumed;
        if (src_len - tpos < 8) return -EINVAL;
        if (load_le32(p + tpos) != ~tee.crc || load_le32(p + tpos + 4) != (uint32_t)tee.size) return -EBADMSG;
        pos = tpos + 8;
        res->consumed = pos;
    }
    return 0;
}
