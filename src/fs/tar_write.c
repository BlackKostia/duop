/* src/fs/tar_write.c - the tar writer (freestanding C23): POSIX ustar headers, and PAX extended headers for what a ustar header cannot hold
 * (a path that does not split into prefix and name, a link target over 100 bytes, a size of 8 GiB or more, an owner id over 2097151, a time
 * that is negative or beyond the 11 octal digits). A reader that knows only ustar still sees a truncated name; every other one sees the
 * full values. Output is whole 512-byte blocks: header, data, padding; two zero blocks end the archive. */
#include "duop_arcpack_int.h"
#include "duop_oop.h"
#include "duop_pal.h"
#include "duop_string.h"

#define OCT_MAX(digits) ((1ull << (3 * (digits))) - 1)

static int emit(TarWriter *t, const void *p, size_t n)
{
    intptr_t r = t->out->write(t->out->ctx, p, n);
    return r < 0 ? (int)r : 0;
}

static void put_octal(char *dst, size_t width, uint64_t v)         /* width - 1 digits (the caller made sure the value fits) and a NUL */
{
    dst[width - 1] = 0;
    for (size_t i = width - 1; i-- > 0;) {
        dst[i] = (char)('0' + (v & 7));
        v >>= 3;
    }
}

static size_t put_decimal(char *dst, int64_t v)
{
    char t[24];
    size_t n = 0, k = 0;
    uint64_t u = v < 0 ? (uint64_t)0 - (uint64_t)v : (uint64_t)v;
    do t[n++] = (char)('0' + u % 10); while (u /= 10);
    if (v < 0) dst[k++] = '-';
    while (n) dst[k++] = t[--n];
    return k;
}

/* appends "<len> key=value\n" (len counts itself) to pax at *used */
static bool pax_record(char *pax, size_t cap, size_t *used, const char *key, const char *val, size_t vlen)
{
    size_t body = str_len(key) + 1 + vlen + 2, digits = 1, total;
    for (;; digits++) {                                          /* the length field has to include its own digits */
        total = body + digits;
        char t[24];
        if (put_decimal(t, (int64_t)total) == digits) break;
    }
    if (*used + total > cap) return false;
    char *p = pax + *used;
    p += put_decimal(p, (int64_t)total);
    *p++ = ' ';
    for (const char *k = key; *k; k++) *p++ = *k;
    *p++ = '=';
    __builtin_memcpy(p, val, vlen);
    p += vlen;
    *p++ = '\n';
    *used += total;
    return true;
}

static bool pax_number(char *pax, size_t cap, size_t *used, const char *key, int64_t v)
{
    char t[24];
    size_t n = put_decimal(t, v);
    return pax_record(pax, cap, used, key, t, n);
}

/* name / prefix fields: the whole name in 100 bytes, or split at a '/' so that the prefix fits 155 and the rest 100; false when it cannot be done */
static bool split_name(const char *name, size_t n, size_t *prefix_len, size_t *name_off)
{
    *prefix_len = 0;
    *name_off = 0;
    if (n <= 100) return true;
    size_t trimmed = n && name[n - 1] == '/' ? n - 1 : n;
    for (size_t i = trimmed; i-- > 0;) {
        if (name[i] == '/' && i <= 155 && n - i - 1 <= 100 && i > 0) {
            *prefix_len = i;
            *name_off = i + 1;
            return true;
        }
    }
    return false;
}

static void checksum(uint8_t *h)
{
    __builtin_memset(h + 148, ' ', 8);
    uint32_t sum = 0;
    for (int i = 0; i < 512; i++) sum += h[i];
    for (int i = 5; i >= 0; i--) {
        h[148 + i] = (uint8_t)('0' + (sum & 7));
        sum >>= 3;
    }
    h[154] = 0;
    h[155] = ' ';
}

static void base_header(uint8_t *h, const char *name, size_t name_n, uint64_t size, uint32_t mode, uint32_t uid, uint32_t gid, int64_t mtime, char type)
{
    __builtin_memset(h, 0, 512);
    __builtin_memcpy(h, name, name_n > 100 ? 100 : name_n);
    put_octal((char *)h + 100, 8, mode & 07777);
    put_octal((char *)h + 108, 8, uid);
    put_octal((char *)h + 116, 8, gid);
    put_octal((char *)h + 124, 12, size);
    put_octal((char *)h + 136, 12, mtime < 0 ? 0 : (uint64_t)mtime);
    h[156] = (uint8_t)type;
    __builtin_memcpy(h + 257, "ustar\0" "00", 8);
    __builtin_memcpy(h + 329, "0000000\0" "0000000\0", 16);
}

int tar_begin(TarWriter *t, const PackEntry *e)
{
    uint8_t h[512];
    size_t used = 0, nlen = str_len(e->name), llen = e->link ? str_len(e->link) : 0, pre, off;
    bool ok = true, split = split_name(e->name, nlen, &pre, &off);
    uint64_t size = e->type == '0' ? e->size : 0;
    bool big_size = size > OCT_MAX(11), big_uid = e->uid > OCT_MAX(7), big_gid = e->gid > OCT_MAX(7), odd_time = e->mtime < 0 || (uint64_t)e->mtime > OCT_MAX(11);
    if (!split) ok = ok && pax_record(t->pax, sizeof t->pax, &used, "path", e->name, nlen);
    if (llen > 100) ok = ok && pax_record(t->pax, sizeof t->pax, &used, "linkpath", e->link, llen);
    if (big_size) ok = ok && pax_number(t->pax, sizeof t->pax, &used, "size", (int64_t)size);
    if (big_uid) ok = ok && pax_number(t->pax, sizeof t->pax, &used, "uid", e->uid);
    if (big_gid) ok = ok && pax_number(t->pax, sizeof t->pax, &used, "gid", e->gid);
    if (odd_time) ok = ok && pax_number(t->pax, sizeof t->pax, &used, "mtime", e->mtime);
    if (!ok) return -ENAMETOOLONG;
    if (used) {                                                  /* the extended header: its own ustar header, then the records */
        char pname[100] = "PaxHeaders.0/";
        const char *base = e->name;                                /* the last component, for the name of the extended header */
        for (size_t i = 0; i < nlen; i++)
            if (e->name[i] == '/' && i + 1 < nlen) base = e->name + i + 1;
        size_t bl = str_len(base) > 80 ? 80 : str_len(base);
        __builtin_memcpy(pname + 13, base, bl);
        base_header(h, pname, 13 + bl, used, 0644, 0, 0, 0, 'x');
        checksum(h);
        int r = emit(t, h, 512);
        if (!r) r = emit(t, t->pax, used);
        if (!r && used % 512) {
            static const uint8_t zeros[512];
            r = emit(t, zeros, 512 - used % 512);
        }
        if (r) return r;
    }
    base_header(h, split ? e->name + off : e->name, split ? nlen - off : nlen, big_size ? 0 : size, e->mode, big_uid ? 0 : e->uid, big_gid ? 0 : e->gid, odd_time ? 0 : e->mtime, e->type);
    if (split && pre) __builtin_memcpy(h + 345, e->name, pre);
    if (e->link) __builtin_memcpy(h + 157, e->link, llen > 100 ? 100 : llen);
    checksum(h);
    t->left = t->size = size;
    return emit(t, h, 512);
}

int tar_data(TarWriter *t, const uint8_t *p, size_t n)
{
    if (n > t->left) n = (size_t)t->left;                        /* a file that grew: only what the header promised */
    t->left -= n;
    return n ? emit(t, p, n) : 0;
}

int tar_end(TarWriter *t)
{
    static const uint8_t zeros[4096];
    while (t->left) {                                            /* a file that shrank: the rest is zeros */
        size_t k = t->left < sizeof zeros ? (size_t)t->left : sizeof zeros;
        int r = emit(t, zeros, k);
        if (r) return r;
        t->left -= k;
    }
    return t->size % 512 ? emit(t, zeros, 512 - t->size % 512) : 0;
}

int tar_finish(TarWriter *t)
{
    static const uint8_t zeros[1024];
    return emit(t, zeros, sizeof zeros);
}

typedef struct { uint8_t *buf; size_t cap, n; } Cap;

static intptr_t cap_write(void *ctx, const uint8_t *p, size_t n)
{
    Cap *c = ctx;
    if (n > c->cap - c->n) n = c->cap - c->n;
    __builtin_memcpy(c->buf + c->n, p, n);
    c->n += n;
    return 0;
}

size_t arc_pack_tar_header_test(uint8_t *out, size_t cap, uint64_t size, uint32_t uid, int64_t mtime, char type, uint32_t mode, const char *name, const char *link)
{
    Cap c = {out, cap, 0};
    CodecSink s = {cap_write, &c};
    TarWriter *t = mem_alloc(sizeof *t);
    if (!t) return 0;
    t->out = &s;
    PackEntry e = {name, link && link[0] ? link : nullptr, size, mtime, mode, uid, uid, type};
    if (tar_begin(t, &e)) c.n = 0;
    mem_free(t);
    return c.n;
}
