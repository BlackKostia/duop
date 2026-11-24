/* src/fs/lzma2_dec.c - LZMA2 chunks over the LZMA decoder (freestanding C23).
 *
 * LZMA2 cuts the data into chunks of at most 2 MiB (unpacked) that are LZMA coded or stored, and says per chunk what is reset: the
 * dictionary, the LZMA state, the lc / lp / pb properties. The dictionary is a ring of the size the block header asked for (rounded up
 * to 16 so that positions keep their low bits); every chunk is delivered to the sink as soon as it is decoded, so a ring smaller than the
 * chunk is fine. The memory of the ring is touched only as far as the data goes. The control bytes are checked as liblzma checks them: the
 * first chunk resets the dictionary, an LZMA chunk needs properties since the last dictionary reset, and nothing may follow a missing end. */
#include "duop_lzma_int.h"
#include "duop_oop.h"
#include "duop_xz_int.h"

int xz_out_write(XzOut *o, const uint8_t *p, size_t n)
{
    o->block_out += n;
    if (o->check) o->check(o->check_ctx, p, n);
    while (n) {
        size_t k = n < CODEC_CHUNK ? n : CODEC_CHUNK;
        uint64_t room = o->max_out - o->produced;
        bool over = k > room;
        if (over) k = (size_t)room;
        if (k) {
            intptr_t r = o->sink->write(o->sink->ctx, p, k);
            if (r < 0) return (int)r;
            o->produced += k;
        }
        if (over) return -EFBIG;
        p += k;
        n -= k;
    }
    return 0;
}

/* delivers [done, pos) and wraps the ring when it is full */
static int flush(Lzma *z, XzOut *o)
{
    int r = xz_out_write(o, z->buf + z->done, z->pos - z->done);
    z->done = z->pos;
    if (z->pos == z->size) z->pos = z->done = 0;
    return r;
}

static int lzma_chunk(Lzma *z, XzOut *o, size_t usize)
{
    while (usize) {
        size_t room = z->size - z->pos, step = usize < room ? usize : room, before = z->pos;
        int r = lzma_run(z, z->pos + step);
        if (r) return r;
        usize -= z->pos - before;
        r = flush(z, o);
        if (r) return r;
    }
    return z->pending || !lzma_rc_finished(z) ? -EINVAL : 0;      /* a match may not cross the chunk; the range coder must end with it */
}

static int stored_chunk(Lzma *z, XzOut *o, const uint8_t *p, size_t n)
{
    while (n) {
        size_t take = n < z->size - z->pos ? n : z->size - z->pos;
        __builtin_memcpy(z->buf + z->pos, p, take);
        z->pos += take;
        if (z->full < z->pos) z->full = z->pos;
        p += take;
        n -= take;
        int r = flush(z, o);
        if (r) return r;
    }
    return 0;
}

/* one chunk: control byte at src[0]; the bytes it takes (0 for the end mark byte handled by the caller) or a negative error */
static int64_t chunk(Lzma *z, XzOut *o, const uint8_t *src, size_t n, bool *need_dict, bool *need_props)
{
    uint32_t ctl = src[0];
    size_t i = 1;
    if (ctl >= 0xE0 || ctl == 1) {                                /* the dictionary starts over */
        *need_props = true;
        *need_dict = false;
        z->pos = z->done = z->full = 0;
    } else if (*need_dict) {
        return -EINVAL;
    }
    if (ctl < 0x80) {                                             /* stored: 1 = with a dictionary reset, 2 = without */
        if (ctl > 2 || n - i < 2) return -EINVAL;
        size_t size = ((size_t)src[1] << 8 | src[2]) + 1;
        i += 2;
        if (n - i < size) return -EINVAL;
        int r = stored_chunk(z, o, src + i, size);
        return r ? r : (int64_t)(i + size);
    }
    if (n - i < 4) return -EINVAL;
    size_t usize = ((size_t)(ctl & 0x1F) << 16 | (size_t)src[1] << 8 | src[2]) + 1, csize = ((size_t)src[3] << 8 | src[4]) + 1;
    i += 4;
    if (ctl >= 0xC0) {                                            /* new properties (and a state reset) */
        if (n - i < 1 || !lzma_set_props(z, src[i++])) return -EINVAL;
        *need_props = false;
        lzma_reset_state(z);
    } else if (*need_props) {
        return -EINVAL;
    } else if (ctl >= 0xA0) {
        lzma_reset_state(z);
    }
    if (n - i < csize || lzma_rc_init(z, src + i, csize)) return -EINVAL;
    int r = lzma_chunk(z, o, usize);
    return r ? r : (int64_t)(i + csize);
}

int lzma2_decode(Lzma *z, XzOut *o, uint32_t dict_size, const uint8_t *src, size_t n, size_t *used)
{
    size_t size = (dict_size < 4096 ? 4096 : (size_t)dict_size + 15) & ~(size_t)15;
    z->buf = mem_alloc(size);
    if (!z->buf) return -ENOMEM;
    z->size = size;
    z->pos = z->done = z->full = 0;
    z->pending = 0;
    bool need_dict = true, need_props = true;
    size_t i = 0;
    int r = -EINVAL;                                              /* the end mark is required */
    while (i < n) {
        if (src[i] == 0) {
            i++;
            r = 0;
            break;
        }
        int64_t c = chunk(z, o, src + i, n - i, &need_dict, &need_props);
        if (c < 0) {
            r = (int)c;
            break;
        }
        i += (size_t)c;
    }
    mem_free(z->buf);
    z->buf = nullptr;
    *used = i;
    return r;
}
