/* src/fs/arc_extract.c - one archive member streamed into a file descriptor, verified on the way (freestanding C23).
 *
 * tar members are copied in place; zip members are stored or inflated (inflate_stream: 64 KiB window, so a member of any size costs
 * the same RAM) with the CRC-32 and the length compared against the central directory. A failed check is -EBADMSG and the caller
 * discards what was written. Methods other than 0 / 8 / 9 / 93 and encrypted members are refused, never guessed. */
#include "duop_arcvfs.h"
#include "duop_hash.h"
#include "duop_inflate.h"
#include "duop_pal.h"
#include "duop_sys.h"
#include "duop_zstd.h"

typedef struct {
    int fd;
    uint32_t crc;                   /* running CRC-32 (raw) */
    uint64_t n;
    ArcXfer *x;
} FdTee;

#define COPY_CHUNK (1u << 20)

static intptr_t tee_write(void *ctx, const uint8_t *buf, size_t len)
{
    FdTee *t = ctx;
    ArcXfer *x = t->x;
    t->crc = crc32_update(t->crc, buf, len);
    t->n += len;
    intptr_t r = 0;
    if (x && x->buf) {                                                         /* into memory */
        if (len > x->cap - x->len) r = -EFBIG;
        else {
            __builtin_memcpy(x->buf + x->len, buf, len);
            x->len += len;
        }
    } else {
        r = sys_write_all(t->fd, buf, len);
        if (r > 0) r = 0;
    }
    if (r == 0 && x && x->progress) r = x->progress(x->ctx, len);
    if (r < 0 && x) x->sink_err = r;
    return r < 0 ? r : 0;
}

/* data already in memory (tar, stored zip): handed over in chunks so progress and cancellation see it */
static intptr_t tee_copy(FdTee *t, const uint8_t *p, uint64_t n)
{
    while (n) {
        size_t c = n < COPY_CHUNK ? (size_t)n : COPY_CHUNK;
        intptr_t r = tee_write(t, p, c);
        if (r) return r;
        p += c;
        n -= c;
    }
    return 0;
}

/* the data of a zip member: behind its local header, at most comp_size bytes and what the file really holds */
static intptr_t zip_data(const ArcSource *s, const ArcItem *it, const uint8_t **p, uint64_t *comp)
{
    const uint8_t *d = s->data;
    uint64_t off = it->off;
    if (off > s->len || s->len - off < 30 || load_le32(d + off) != 0x04034B50u) return -EINVAL;
    uint64_t hdr = 30 + (uint64_t)load_le16(d + off + 26) + load_le16(d + off + 28);
    if (hdr > s->len - off) return -EINVAL;
    uint64_t avail = s->len - off - hdr;
    *p = d + off + hdr;
    *comp = it->comp_size < avail ? it->comp_size : avail;
    return 0;
}

intptr_t arc_extract_to(const ArcSource *s, const ArcItem *it, int fd, ArcXfer *x)
{
    if (x) x->sink_err = 0;
    if (it->flags & (ARC_F_DIR | ARC_F_UNSAFE | ARC_F_SPECIAL)) return -EINVAL;
    if (it->flags & ARC_F_ENCRYPTED) return -EACCES;
    if (it->flags & ARC_F_TRUNCATED) return -EBADMSG;                          /* the archive ends inside this member */
    if ((it->flags & (ARC_F_SYMLINK | ARC_F_HARDLINK)) && !(x && x->buf)) return -EOPNOTSUPP;
    FdTee t = {fd, 0xFFFFFFFFu, 0, x};
    if (s->type != ARC_ZIP) {                                                  /* tar: the bytes in place */
        if (it->off > s->len || it->size > s->len - it->off) return -EINVAL;
        return tee_copy(&t, s->data + it->off, it->size);
    }
    const uint8_t *p;
    uint64_t comp;
    intptr_t r = zip_data(s, it, &p, &comp);
    if (r) return r;
    CodecSink sink = {tee_write, &t};
    if (it->method == 0) {
        if (it->size > comp) return -EINVAL;
        r = tee_copy(&t, p, it->size);
    } else if (it->method == 8 || it->method == 9) {
        CodecResult res;
        r = inflate_stream(p, comp, it->method == 9 ? INFLATE_DEFLATE64 : 0, it->size, &sink, &res);
    } else if (it->method == 93) {                                            /* Zstandard */
        CodecResult res;
        r = zstd_decode(p, comp, it->size, &sink, &res);
    } else {
        return -EOPNOTSUPP;
    }
    if (r) return r;
    if (t.n != it->size) return -EBADMSG;
    if (~t.crc != it->crc32) return -EBADMSG;
    return 0;
}

intptr_t arc_extract_item(const ArcSource *s, const ArcItem *it, int fd) { return arc_extract_to(s, it, fd, nullptr); }
