/* src/fs/arc_tar.c - the tar parser of the archive layer (freestanding C23): ustar, GNU and PAX archives into an ArcIndex.
 *
 * Handled: the 100-byte name and the ustar prefix (POSIX magic only: in a GNU header those bytes mean something else), GNU long names
 * and link targets (types L / K), PAX extended headers (x: path, linkpath, size, mtime, GNU.sparse.*; g is validated and ignored),
 * octal and base-256 numbers, hard / symbolic links, directories, devices / fifos (listed, flagged ARC_F_SPECIAL), GNU sparse members
 * (listed, flagged and counted as unsupported). Header checksums are verified (unsigned and signed sums are both accepted, as
 * tar does): the first header failing means "not a tar" (-EINVAL), a later one ends the parse with `damaged` set and the members
 * before it kept. The data is untrusted: every length is compared with what remains, sizes are clamped to the data present. */
#include "duop_arcparse.h"
#include "duop_oop.h"
#include "duop_pal.h"
#include "duop_string.h"

typedef struct {                    /* what x / L / K headers say about the next member (pointers into the archive, no copies) */
    const uint8_t *path, *link;
    uint64_t path_len, link_len, size;
    int64_t mtime;
    bool has_path, has_link, has_size, has_mtime;
} Ext;

static bool bytes_zero(const uint8_t *p, uint64_t n)
{
    for (uint64_t i = 0; i < n; i++)
        if (p[i]) return false;
    return true;
}

/* an octal number (leading blanks skipped, ends at the first other byte) or a GNU base-256 one (high bit set); false: negative / > 2^63 */
static bool tar_num(const uint8_t *p, int n, uint64_t *out)
{
    uint64_t v = 0;
    if (p[0] & 0x80) {
        if (p[0] == 0xFF) return false;
        v = p[0] & 0x7Fu;
        for (int i = 1; i < n; i++) {
            if (v >> 55) return false;
            v = v << 8 | p[i];
        }
        *out = v;
        return true;
    }
    int i = 0;
    while (i < n && p[i] == ' ') i++;
    for (; i < n && p[i] >= '0' && p[i] <= '7'; i++) v = v << 3 | (uint64_t)(p[i] - '0');
    *out = v;
    return true;
}

static bool tar_checksum_ok(const uint8_t *h)
{
    uint64_t want;
    if (!tar_num(h + 148, 8, &want)) return false;
    uint32_t sum_u = 0;
    int32_t sum_s = 0;
    for (int i = 0; i < 512; i++) {
        uint8_t c = i >= 148 && i < 156 ? ' ' : h[i];                         /* the checksum field counts as blanks */
        sum_u += c;
        sum_s += (int8_t)c;
    }
    return want == sum_u || (int64_t)want == sum_s;
}

static size_t field_len(const uint8_t *p, size_t max)
{
    size_t n = 0;
    while (n < max && p[n]) n++;
    return n;
}

/* a decimal number filling v[0, n); false when empty, with another byte, or beyond 2^63 */
static bool dec_all(const uint8_t *v, uint64_t n, uint64_t *out)
{
    uint64_t r = 0;
    if (!n) return false;
    for (uint64_t i = 0; i < n; i++) {
        if (v[i] < '0' || v[i] > '9' || r > (UINT64_MAX >> 1) / 10) return false;
        r = r * 10 + (uint64_t)(v[i] - '0');
    }
    *out = r;
    return true;
}

static bool pax_apply(Ext *ext, const uint8_t *key, uint64_t kl, const uint8_t *val, uint64_t vl)
{
    if (kl == 4 && !__builtin_memcmp(key, "path", 4)) {
        ext->path = val;
        ext->path_len = vl;
        ext->has_path = true;
    } else if (kl == 8 && !__builtin_memcmp(key, "linkpath", 8)) {
        ext->link = val;
        ext->link_len = vl;
        ext->has_link = true;
    } else if (kl == 4 && !__builtin_memcmp(key, "size", 4)) {
        if (!dec_all(val, vl, &ext->size)) return false;
        ext->has_size = true;
    } else if (kl == 5 && !__builtin_memcmp(key, "mtime", 5)) {
        bool neg = vl && val[0] == '-';
        uint64_t ip = 0, i = neg;
        while (i < vl && val[i] != '.') i++;                                   /* the fractional part is dropped */
        if (!dec_all(val + neg, i - neg, &ip)) return false;
        ext->mtime = neg ? -(int64_t)ip : (int64_t)ip;
        ext->has_mtime = true;
    }
    return true;
}

/* "<len> <key>=<value>\n" records; false when one is malformed */
static bool pax_parse(const uint8_t *d, uint64_t n, Ext *ext)
{
    uint64_t pos = 0;
    while (pos < n) {
        const uint8_t *p = d + pos;
        uint64_t rem = n - pos, rl = 0, i = 0;
        while (i < rem && p[i] >= '0' && p[i] <= '9' && i < 12) rl = rl * 10 + (uint64_t)(p[i++] - '0');
        if (i == 0 || i >= rem || p[i] != ' ') return false;
        if (rl < 4 || rl > rem) return false;
        const uint8_t *rec = p + i + 1;
        uint64_t rlen = rl - i - 1;                                            /* key=value\n */
        if (rlen < 2 || rec[rlen - 1] != '\n') return false;
        rlen--;
        uint64_t eq = 0;
        while (eq < rlen && rec[eq] != '=') eq++;
        if (eq == rlen) return false;
        if (!pax_apply(ext, rec, eq, rec + eq + 1, rlen - eq - 1)) return false;
        pos += rl;
    }
    return true;
}

/* x / g / L / K data: 0, or -1 for malformed records */
static int tar_meta(Ext *ext, char type, const uint8_t *d, uint64_t n)
{
    Ext scratch = {0};
    switch (type) {
    case 'x': return pax_parse(d, n, ext) ? 0 : -1;
    case 'g': return pax_parse(d, n, &scratch) ? 0 : -1;                      /* validated, not applied */
    case 'L':
        ext->path = d;
        ext->path_len = field_len(d, n);
        ext->has_path = true;
        return 0;
    default:
        ext->link = d;
        ext->link_len = field_len(d, n);
        ext->has_link = true;
        return 0;
    }
}

/* the member's name: a PAX path / GNU long name, else ustar prefix + '/' + name */
static size_t tar_name(const uint8_t *h, const Ext *ext, char *buf, const char **out)
{
    if (ext->has_path) {
        *out = (const char *)ext->path;
        return (size_t)ext->path_len;
    }
    bool posix = !__builtin_memcmp(h + 257, "ustar", 5) && h[262] == 0;
    size_t k = 0;
    if (posix && h[345]) {
        k = field_len(h + 345, 155);
        __builtin_memcpy(buf, h + 345, k);
        buf[k++] = '/';
    }
    size_t n = field_len(h, 100);
    __builtin_memcpy(buf + k, h, n);
    *out = buf;
    return k + n;
}

static int tar_add(ArcIndex *x, const uint8_t *h, const Ext *ext, char type, uint64_t data_off, uint64_t size, ArcParseInfo *info)
{
    ArcItem pr = {0};
    uint64_t v;
    pr.off = data_off;
    pr.size = pr.comp_size = size;
    pr.mtime = ext->has_mtime ? ext->mtime : tar_num(h + 136, 12, &v) ? (int64_t)v : 0;
    pr.mode = tar_num(h + 100, 8, &v) ? (uint32_t)(v & 07777) : 0;
    if (type == '1') pr.flags |= ARC_F_HARDLINK;
    if (type == '2') pr.flags |= ARC_F_SYMLINK;
    if (type == '3' || type == '4' || type == '6') pr.flags |= ARC_F_SPECIAL;
    if (type == '5') pr.flags |= ARC_F_DIR;
    if (type == 'S') {
        pr.flags |= ARC_F_SPECIAL;
        info->unsupported++;
    }
    char buf[300];
    const char *name, *link = nullptr;
    size_t nl = tar_name(h, ext, buf, &name), ll = 0;
    if (type == '1' || type == '2') {
        link = ext->has_link ? (const char *)ext->link : (const char *)h + 157;
        ll = ext->has_link ? (size_t)ext->link_len : field_len(h + 157, 100);
    }
    int r = arc_idx_add(x, name, nl, link, ll, &pr);
    if (r == -ENOMEM || r == -EFBIG) return r;
    if (r) info->skipped++;
    return 0;
}

int arc_tar_parse(ArcIndex *x, const uint8_t *data, uint64_t len, ArcParseInfo *info)
{
    mem_zero(info, sizeof *info);
    Ext ext = {0};
    uint64_t off = 0;
    for (bool first = true;; first = false) {
        if (len - off < 512) {
            if (!bytes_zero(data + off, len - off)) {                            /* a torn header: a stub is no tar at all */
                if (first) return -EINVAL;
                info->damaged = 1;
            }
            break;
        }
        const uint8_t *h = data + off;
        if (bytes_zero(h, 512)) break;                                        /* the end-of-archive block */
        if (!tar_checksum_ok(h)) {
            if (first) return -EINVAL;
            info->damaged = 1;
            break;
        }
        uint64_t size, data_off = off + 512, avail = len - data_off;
        char type = (char)h[156];
        if (!tar_num(h + 124, 12, &size)) {
            info->damaged = 1;
            break;
        }
        if (type == 'x' || type == 'g' || type == 'L' || type == 'K') {
            if (size > avail || tar_meta(&ext, type, data + data_off, size) < 0) {
                info->damaged = 1;
                break;
            }
            uint64_t padded = (size + 511) & ~(uint64_t)511;
            off = padded > avail ? len : data_off + padded;
            continue;
        }
        if (ext.has_size) size = ext.size;
        if (type >= '1' && type <= '6') size = 0;                             /* links, devices, directories, fifos carry no data */
        if (size > avail) {
            size = avail;
            info->damaged = 1;
        }
        if (type != 'V' && type != 'D' && type != 'M' && type != 'N') {
            int r = tar_add(x, h, &ext, type, data_off, size, info);
            if (r) return r;
        }
        ext = (Ext){0};
        uint64_t padded = (size + 511) & ~(uint64_t)511;
        off = padded > avail ? len : data_off + padded;
    }
    info->end = off;
    return 0;
}
