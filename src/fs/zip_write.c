/* src/fs/zip_write.c - the zip writer (freestanding C23): entries are streamed - local header with a data descriptor behind the data - and the
 * central directory is written last from a table kept in memory, so a file of any size costs the same RAM. Names are UTF-8 (flag bit 11),
 * each entry carries the extended timestamp (0x5455) and its Unix mode in the external attributes. An entry of ARC_PACK_ZIP64_AT bytes or more
 * gets ZIP64 records (64-bit sizes in the descriptor and the central directory); so does an archive with 65535 entries or offsets beyond 4 GiB. */
#include "duop_arcpack_int.h"
#include "duop_hash.h"
#include "duop_oop.h"
#include "duop_pal.h"
#include "duop_string.h"

typedef struct ZEnt {
    uint64_t off, csize, usize;
    uint32_t name_off, name_len, crc, attr;
    uint16_t method, time, date;
    int64_t mtime;
    bool z64;
} ZEnt;

struct ZipWriter {
    const CodecSink *out;
    int level;
    Deflate *def;
    CodecSink dsink;                   /* what the compressor writes to */
    uint64_t pos;                      /* bytes written so far */
    ZEnt *ents;
    uint32_t count, cap;
    char *names;
    size_t names_len, names_cap;
    ZEnt cur;                          /* the entry being written */
    bool open;
};

static int emit(ZipWriter *z, const void *p, size_t n)
{
    intptr_t r = z->out->write(z->out->ctx, p, n);
    if (r < 0) return (int)r;
    z->pos += n;
    return 0;
}

static intptr_t def_sink(void *ctx, const uint8_t *p, size_t n)
{
    ZipWriter *z = ctx;
    int r = emit(z, p, n);
    z->cur.csize += n;
    return r;
}

static void put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, v & 0xFFFF); put16(p + 2, v >> 16); }
static void put64(uint8_t *p, uint64_t v) { put32(p, (uint32_t)v); put32(p + 4, (uint32_t)(v >> 32)); }

/* DOS date and time of a Unix time (UTC; 1980 is the earliest and 2107 the latest), by the civil-from-days algorithm */
static void dos_time(int64_t t, uint16_t *dt, uint16_t *dd)
{
    if (t < 315532800) t = 315532800;
    int64_t days = t / 86400, rem = t % 86400, z = days + 719468, era = z / 146097, doe = z - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365, doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
    int64_t day = doy - (153 * mp + 2) / 5 + 1, month = mp < 10 ? mp + 3 : mp - 9, year = yoe + era * 400 + (month <= 2);
    if (year > 2107) year = 2107;
    *dt = (uint16_t)(rem / 3600 << 11 | rem % 3600 / 60 << 5 | rem % 60 / 2);
    *dd = (uint16_t)((year - 1980) << 9 | month << 5 | day);
}

ZipWriter *zip_new(const CodecSink *out, int level)
{
    ZipWriter *z = mem_alloc(sizeof *z);
    if (!z) return nullptr;
    z->out = out;
    z->level = level;
    return z;
}

void zip_free(ZipWriter *z)
{
    if (!z) return;
    deflate_free(z->def);
    mem_free(z->ents);
    mem_free(z->names);
    mem_free(z);
}

/* the timestamp extra field: id, size 5, flags 1 (mtime), mtime */
static size_t put_ut(uint8_t *p, int64_t mtime)
{
    put16(p, 0x5455);
    put16(p + 2, 5);
    p[4] = 1;
    put32(p + 5, (uint32_t)(mtime < 0 ? 0 : mtime));
    return 9;
}

int zip_begin(ZipWriter *z, const PackEntry *e)
{
    ZEnt *c = &z->cur;
    __builtin_memset(c, 0, sizeof *c);
    bool dir = e->type == '5', link = e->type == '2';
    size_t nlen = str_len(e->name);
    if (nlen > 0xFFFF) return -ENAMETOOLONG;
    uint64_t expect = link ? str_len(e->link) : e->size;
    c->z64 = expect >= ARC_PACK_ZIP64_AT;
    c->method = z->level > 0 && !dir && expect > 0 ? 8 : 0;
    c->off = z->pos;
    c->crc = 0xFFFFFFFFu;
    c->mtime = e->mtime;
    dos_time(e->mtime, &c->time, &c->date);
    c->attr = ((link ? 0120000u : dir ? 040000u : 0100000u) | (e->mode & 07777)) << 16 | (dir ? 0x10u : 0);
    if (z->names_len + nlen + 1 > z->names_cap) {
        size_t cap = z->names_cap ? z->names_cap * 2 : 4096;
        while (cap < z->names_len + nlen + 1) cap *= 2;
        char *n = mem_realloc(z->names, cap);
        if (!n) return -ENOMEM;
        z->names = n;
        z->names_cap = cap;
    }
    if (z->count == z->cap) {
        uint32_t cap = z->cap ? z->cap * 2 : 256;
        ZEnt *n = mem_realloc(z->ents, (size_t)cap * sizeof *n);
        if (!n) return -ENOMEM;
        z->ents = n;
        z->cap = cap;
    }
    __builtin_memcpy(z->names + z->names_len, e->name, nlen);
    c->name_off = (uint32_t)z->names_len;
    c->name_len = (uint32_t)nlen;
    z->names_len += nlen;
    if (c->method == 8) {
        z->dsink = (CodecSink){def_sink, z};
        if (!z->def) z->def = deflate_new(z->level, &z->dsink);
        if (!z->def) return -ENOMEM;
        deflate_reset(z->def);
    }
    uint8_t h[30 + 9 + 20];
    size_t extra = put_ut(h + 30, e->mtime);
    if (c->z64) {
        put16(h + 30 + extra, 1);
        put16(h + 32 + extra, 16);
        put64(h + 34 + extra, 0);
        put64(h + 42 + extra, 0);
        extra += 20;
    }
    put32(h, 0x04034B50);
    put16(h + 4, c->z64 ? 45 : c->method ? 20 : 10);
    put16(h + 6, 0x0808);                                         /* data descriptor, UTF-8 names */
    put16(h + 8, c->method);
    put16(h + 10, c->time);
    put16(h + 12, c->date);
    put32(h + 14, 0);
    put32(h + 18, c->z64 ? 0xFFFFFFFFu : 0);
    put32(h + 22, c->z64 ? 0xFFFFFFFFu : 0);
    put16(h + 26, (uint32_t)nlen);
    put16(h + 28, (uint32_t)extra);
    int r = emit(z, h, 30);
    if (!r) r = emit(z, e->name, nlen);
    if (!r) r = emit(z, h + 30, extra);
    z->open = !r;
    return r;
}

int zip_data(ZipWriter *z, const uint8_t *p, size_t n)
{
    ZEnt *c = &z->cur;
    c->crc = crc32_update(c->crc, p, n);
    c->usize += n;
    if (c->method == 8) return deflate_write(z->def, p, n);
    c->csize += n;
    return emit(z, p, n);
}

int zip_end(ZipWriter *z)
{
    ZEnt *c = &z->cur;
    if (c->method == 8) {
        int r = deflate_finish(z->def);
        if (r) return r;
    }
    c->crc = ~c->crc;
    if (c->usize >= ARC_PACK_ZIP64_AT || c->csize >= ARC_PACK_ZIP64_AT) c->z64 = true;
    uint8_t d[24];
    put32(d, 0x08074B50);
    put32(d + 4, c->crc);
    size_t n;
    if (c->z64) {
        put64(d + 8, c->csize);
        put64(d + 16, c->usize);
        n = 24;
    } else {
        put32(d + 8, (uint32_t)c->csize);
        put32(d + 12, (uint32_t)c->usize);
        n = 16;
    }
    int r = emit(z, d, n);
    z->ents[z->count++] = *c;
    z->open = false;
    return r;
}

static int central_entry(ZipWriter *z, const ZEnt *c)
{
    uint8_t h[46 + 9 + 28];
    uint8_t zx[28];
    size_t zn = 0;
    bool big_off = c->off >= 0xFFFFFFFFull, z64 = c->z64 || big_off;
    if (c->z64) {
        put64(zx + zn, c->usize);
        put64(zx + zn + 8, c->csize);
        zn += 16;
    }
    if (big_off) {
        put64(zx + zn, c->off);
        zn += 8;
    }
    size_t extra = put_ut(h + 46, c->mtime);
    if (zn) {
        put16(h + 46 + extra, 1);
        put16(h + 48 + extra, (uint32_t)zn);
        __builtin_memcpy(h + 50 + extra, zx, zn);
        extra += 4 + zn;
    }
    put32(h, 0x02014B50);
    put16(h + 4, 0x033F);                                         /* made by Unix, version 6.3 */
    put16(h + 6, z64 ? 45 : c->method ? 20 : 10);
    put16(h + 8, 0x0808);
    put16(h + 10, c->method);
    put16(h + 12, c->time);
    put16(h + 14, c->date);
    put32(h + 16, c->crc);
    put32(h + 20, c->z64 ? 0xFFFFFFFFu : (uint32_t)c->csize);
    put32(h + 24, c->z64 ? 0xFFFFFFFFu : (uint32_t)c->usize);
    put16(h + 28, c->name_len);
    put16(h + 30, (uint32_t)extra);
    put16(h + 32, 0);
    put16(h + 34, 0);
    put16(h + 36, 0);
    put32(h + 38, c->attr);
    put32(h + 42, big_off ? 0xFFFFFFFFu : (uint32_t)c->off);
    int r = emit(z, h, 46);
    if (!r) r = emit(z, z->names + c->name_off, c->name_len);
    return r ? r : emit(z, h + 46, extra);
}

int zip_finish(ZipWriter *z)
{
    uint64_t cd_off = z->pos;
    for (uint32_t i = 0; i < z->count; i++) {
        int r = central_entry(z, &z->ents[i]);
        if (r) return r;
    }
    uint64_t cd_size = z->pos - cd_off, n = z->count;
    bool z64 = n >= 0xFFFF || cd_off >= 0xFFFFFFFFull || cd_size >= 0xFFFFFFFFull;
    uint8_t t[56 + 20 + 22];
    size_t k = 0;
    if (z64) {
        uint64_t rec = z->pos;
        put32(t, 0x06064B50);
        put64(t + 4, 44);
        put16(t + 12, 0x033F);
        put16(t + 14, 45);
        put32(t + 16, 0);
        put32(t + 20, 0);
        put64(t + 24, n);
        put64(t + 32, n);
        put64(t + 40, cd_size);
        put64(t + 48, cd_off);
        put32(t + 56, 0x07064B50);
        put32(t + 60, 0);
        put64(t + 64, rec);
        put32(t + 72, 1);
        k = 76;
    }
    put32(t + k, 0x06054B50);
    put16(t + k + 4, 0);
    put16(t + k + 6, 0);
    put16(t + k + 8, z64 ? 0xFFFF : (uint32_t)n);
    put16(t + k + 10, z64 ? 0xFFFF : (uint32_t)n);
    put32(t + k + 12, z64 ? 0xFFFFFFFFu : (uint32_t)cd_size);
    put32(t + k + 16, z64 ? 0xFFFFFFFFu : (uint32_t)cd_off);
    put16(t + k + 20, 0);
    return emit(z, t, k + 22);
}
