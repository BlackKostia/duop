/* src/fs/arc_zip.c - the zip parser of the archive layer (freestanding C23): the central directory of a zip / ZIP64 file into an ArcIndex.
 *
 * The end record is searched from the back (a comment length that reaches exactly the end wins over a fake record inside the
 * comment), ZIP64 records and extra fields are honoured, and a prefix before the zip (a self-extractor stub, offsets relative to the
 * zip) is detected from where the central directory really is. Per member: UTF-8 names (flag, Info-ZIP 0x7075 path, or valid UTF-8
 * without the flag) or CP437 converted to UTF-8; 0x5455 / DOS times; Unix modes, symlinks and special files from the external
 * attributes, DOS directory bit; encryption flags (traditional, AES 0x9901). A member whose local header is missing is dropped
 * (damaged), sizes are clamped to the data present, and members whose bytes overlap an earlier one are flagged ARC_F_UNSAFE (the
 * overlapping-entries zip bomb). The input is untrusted: every offset is compared against what remains, never summed first. */
#include "duop_arcparse.h"
#include "duop_hash.h"
#include "duop_oop.h"
#include "duop_pal.h"
#include "duop_string.h"

#define SIG_EOCD 0x06054B50u
#define SIG_CD 0x02014B50u
#define SIG_LH 0x04034B50u
#define SIG_Z64_EOCD 0x06064B50u
#define SIG_Z64_LOC 0x07064B50u

typedef struct {
    uint64_t start, limit, total;   /* the central directory: [start, limit), at most `total` records */
    int64_t shift;                  /* bytes in front of the zip: added to every stored offset */
} ZipDir;

static const uint16_t cp437[128] = {
    0x00C7, 0x00FC, 0x00E9, 0x00E2, 0x00E4, 0x00E0, 0x00E5, 0x00E7, 0x00EA, 0x00EB, 0x00E8, 0x00EF, 0x00EE, 0x00EC, 0x00C4, 0x00C5,
    0x00C9, 0x00E6, 0x00C6, 0x00F4, 0x00F6, 0x00F2, 0x00FB, 0x00F9, 0x00FF, 0x00D6, 0x00DC, 0x00A2, 0x00A3, 0x00A5, 0x20A7, 0x0192,
    0x00E1, 0x00ED, 0x00F3, 0x00FA, 0x00F1, 0x00D1, 0x00AA, 0x00BA, 0x00BF, 0x2310, 0x00AC, 0x00BD, 0x00BC, 0x00A1, 0x00AB, 0x00BB,
    0x2591, 0x2592, 0x2593, 0x2502, 0x2524, 0x2561, 0x2562, 0x2556, 0x2555, 0x2563, 0x2551, 0x2557, 0x255D, 0x255C, 0x255B, 0x2510,
    0x2514, 0x2534, 0x252C, 0x251C, 0x2500, 0x253C, 0x255E, 0x255F, 0x255A, 0x2554, 0x2569, 0x2566, 0x2560, 0x2550, 0x256C, 0x2567,
    0x2568, 0x2564, 0x2565, 0x2559, 0x2558, 0x2552, 0x2553, 0x256B, 0x256A, 0x2518, 0x250C, 0x2588, 0x2584, 0x258C, 0x2590, 0x2580,
    0x03B1, 0x00DF, 0x0393, 0x03C0, 0x03A3, 0x03C3, 0x00B5, 0x03C4, 0x03A6, 0x0398, 0x03A9, 0x03B4, 0x221E, 0x03C6, 0x03B5, 0x2229,
    0x2261, 0x00B1, 0x2265, 0x2264, 0x2320, 0x2321, 0x00F7, 0x2248, 0x00B0, 0x2219, 0x00B7, 0x221A, 0x207F, 0x00B2, 0x25A0, 0x00A0,
};

/* the end record: from the back, an exact comment length first, else the nearest one with a comment that fits */
static int64_t find_eocd(const uint8_t *d, uint64_t len)
{
    if (len < 22) return -EINVAL;
    uint64_t lo = len > 22 + 65535 ? len - 22 - 65535 : 0;
    int64_t loose = -1;
    for (uint64_t p = len - 22 + 1; p-- > lo;) {
        if (load_le32(d + p) != SIG_EOCD) continue;
        uint64_t cl = load_le16(d + p + 20);
        if (p + 22 + cl == len) return (int64_t)p;
        if (loose < 0 && p + 22 + cl < len) loose = (int64_t)p;
    }
    return loose >= 0 ? loose : -EINVAL;
}

/* the ZIP64 end record behind the locator (where the locator says, else right before it): its start, or UINT64_MAX */
static uint64_t z64_record(const uint8_t *d, uint64_t len, uint64_t eocd)
{
    if (eocd < 20 || load_le32(d + eocd - 20) != SIG_Z64_LOC) return UINT64_MAX;
    uint64_t at[2] = {load_le64(d + eocd - 12), eocd >= 76 ? eocd - 76 : UINT64_MAX};
    for (int i = 0; i < 2; i++)
        if (at[i] <= len && len - at[i] >= 56 && load_le32(d + at[i]) == SIG_Z64_EOCD) return at[i];
    return UINT64_MAX;
}

static int zip_dir(const uint8_t *d, uint64_t len, uint64_t eocd, ZipDir *z)
{
    const uint8_t *e = d + eocd;
    uint64_t total = load_le16(e + 10), cd_size = load_le32(e + 12), cd_off = load_le32(e + 16), limit = eocd;
    uint32_t disk = load_le16(e + 4), cd_disk = load_le16(e + 6);
    if (total == 0xFFFF || cd_size == 0xFFFFFFFFu || cd_off == 0xFFFFFFFFu) {
        uint64_t zp = z64_record(d, len, eocd);
        if (zp != UINT64_MAX) {
            const uint8_t *r = d + zp;
            disk = load_le32(r + 16);
            cd_disk = load_le32(r + 20);
            total = load_le64(r + 32);
            cd_size = load_le64(r + 40);
            cd_off = load_le64(r + 48);
            limit = zp;
        }
    }
    if ((disk && disk != 0xFFFF) || (cd_disk && cd_disk != 0xFFFF)) return -EOPNOTSUPP;
    z->total = total;
    z->shift = 0;
    z->start = z->limit = limit;
    if (!total) return 0;
    if (cd_off <= limit && cd_size <= limit - cd_off && limit - cd_off >= 4 && load_le32(d + cd_off) == SIG_CD) {
        z->start = cd_off;                                                    /* where it says */
        return 0;
    }
    if (cd_size <= limit && limit - cd_size >= cd_off && limit - cd_size <= len - 4 && load_le32(d + limit - cd_size) == SIG_CD) {
        z->start = limit - cd_size;                                           /* right before the end record: bytes precede the zip */
        z->shift = (int64_t)(z->start - cd_off);
        return 0;
    }
    return -EINVAL;
}

static bool utf8_valid(const uint8_t *p, uint64_t n)
{
    for (uint64_t i = 0; i < n;) {
        uint8_t c = p[i];
        if (c < 0x80) {
            i++;
            continue;
        }
        uint32_t k = c >= 0xC2 && c <= 0xDF ? 1 : c >= 0xE0 && c <= 0xEF ? 2 : c >= 0xF0 && c <= 0xF4 ? 3 : 9;
        if (k == 9 || n - i <= k) return false;
        uint8_t c1 = p[i + 1];
        if ((c == 0xE0 && c1 < 0xA0) || (c == 0xED && c1 > 0x9F) || (c == 0xF0 && c1 < 0x90) || (c == 0xF4 && c1 > 0x8F)) return false;
        for (uint32_t j = 1; j <= k; j++)
            if ((p[i + j] & 0xC0) != 0x80) return false;
        i += k + 1;
    }
    return true;
}

/* a member's name as UTF-8: the Info-ZIP path / the flagged or valid UTF-8 bytes in place, CP437 converted into buf; length, or SIZE_MAX if it does not fit */
static size_t zip_name(const uint8_t *nm, uint64_t n, uint32_t gp, const uint8_t *uni, uint64_t ulen, char *buf, size_t cap, const char **out)
{
    if (uni) {
        *out = (const char *)uni;
        return ulen;
    }
    if ((gp & 0x800) || utf8_valid(nm, n)) {
        *out = (const char *)nm;
        return n;
    }
    size_t k = 0;
    for (uint64_t i = 0; i < n; i++) {
        if (k + 4 > cap) return SIZE_MAX;
        if (nm[i] < 0x80) buf[k++] = (char)nm[i];
        else k += (size_t)utf8_encode(cp437[nm[i] - 0x80], buf + k);
    }
    *out = buf;
    return k;
}

static int64_t dos_time(uint32_t t, uint32_t d)
{
    int64_t y = 1980 + (d >> 9), m = (d >> 5) & 15, day = d & 31;
    if (m < 1 || m > 12 || !day) return 0;
    y -= m <= 2;                                                              /* days from civil (proleptic Gregorian) */
    int64_t era = y / 400, yoe = y - era * 400, doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + day - 1, doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (era * 146097 + doe - 719468) * 86400 + (int64_t)(t >> 11) * 3600 + (int64_t)((t >> 5) & 63) * 60 + (int64_t)(t & 31) * 2;
}

typedef struct {
    uint64_t size, comp, lho;
    int64_t mtime;
    const uint8_t *uni;
    uint64_t ulen;
    bool has_mtime, aes;
} Extras;

/* the extra fields of a central record: false when the ZIP64 field is too short for the values that point to it */
static bool zip_extras(const uint8_t *x, uint64_t n, const uint8_t *name, uint64_t nlen, Extras *ex, bool need_size, bool need_comp, bool need_off)
{
    while (n >= 4) {
        uint32_t id = load_le16(x), sz = load_le16(x + 2);
        x += 4;
        n -= 4;
        if (sz > n) break;
        if (id == 1) {
            uint64_t *f[3] = {&ex->size, &ex->comp, &ex->lho};
            bool need[3] = {need_size, need_comp, need_off};
            uint32_t used = 0;
            for (int i = 0; i < 3; i++) {
                if (!need[i]) continue;
                if (sz - used < 8) return false;
                *f[i] = load_le64(x + used);
                used += 8;
            }
        } else if (id == 0x5455 && sz >= 5 && (x[0] & 1)) {
            ex->mtime = load_le32(x + 1);
            ex->has_mtime = true;
        } else if (id == 0x7075 && sz >= 5 && x[0] == 1 && load_le32(x + 1) == (~crc32_update(0xFFFFFFFFu, name, nlen))) {
            ex->uni = x + 5;
            ex->ulen = sz - 5;
        } else if (id == 0x9901) {
            ex->aes = true;
        }
        x += sz;
        n -= sz;
    }
    return true;
}

/* the type, mode and flags a member's attributes give */
static uint16_t zip_kind(uint32_t made, uint32_t ea, uint32_t *mode)
{
    uint32_t um = ea >> 16, type = um & 0170000, fl = 0;
    bool unix_attr = (made >> 8) == 3 && um;
    *mode = unix_attr ? um & 07777 : 0;
    if (unix_attr) {
        if (type == 0120000) fl |= ARC_F_SYMLINK;
        else if (type == 0010000 || type == 0020000 || type == 0060000 || type == 0140000) fl |= ARC_F_SPECIAL;
        else if (type == 0040000) fl |= ARC_F_DIR;
    }
    if (ea & 0x10) fl |= ARC_F_DIR;
    if (!unix_attr) *mode = (fl & ARC_F_DIR) ? 0755 : 0644;
    return (uint16_t)fl;
}

/* one central record at r (avail bytes remain in the directory): 0 ok / dropped (info says which), -ENOMEM / -EFBIG fatal, 1 = record out of bounds */
static int zip_record(ArcIndex *x, const uint8_t *d, uint64_t len, const uint8_t *r, uint64_t avail, int64_t shift, ArcParseInfo *info, uint64_t *used)
{
    uint64_t nlen = load_le16(r + 28), xlen = load_le16(r + 30), clen = load_le16(r + 32);
    if (avail - 46 < nlen || avail - 46 - nlen < xlen || avail - 46 - nlen - xlen < clen) return 1;
    *used = 46 + nlen + xlen + clen;
    uint32_t gp = load_le16(r + 8), method = load_le16(r + 10);
    ArcItem pr = {0};
    Extras ex = {load_le32(r + 24), load_le32(r + 20), load_le32(r + 42), 0, nullptr, 0, false, false};
    bool ok = zip_extras(r + 46 + nlen, xlen, r + 46, nlen, &ex, ex.size == 0xFFFFFFFFu, ex.comp == 0xFFFFFFFFu, ex.lho == 0xFFFFFFFFu);
    if (!ok || ex.lho > len || len - ex.lho < (uint64_t)shift) { info->damaged = 1; return 0; }
    uint64_t lho = ex.lho + (uint64_t)shift;                                  /* the local header */
    if (len - lho < 30 || load_le32(d + lho) != SIG_LH) { info->damaged = 1; return 0; }
    uint64_t hdr = 30 + (uint64_t)load_le16(d + lho + 26) + load_le16(d + lho + 28);
    if (hdr > len - lho) { info->damaged = 1; return 0; }
    bool torn = ex.comp > len - lho - hdr;
    if (torn) {                                                               /* data beyond the file: what is there */
        ex.comp = len - lho - hdr;
        info->damaged = 1;
    }
    if (!method && ex.size > ex.comp) ex.size = ex.comp;
    pr.off = lho;
    if (torn) pr.flags |= ARC_F_TRUNCATED;
    pr.size = ex.size;
    pr.comp_size = ex.comp;
    pr.mtime = ex.has_mtime ? ex.mtime : dos_time(load_le16(r + 12), load_le16(r + 14));
    pr.crc32 = load_le32(r + 16);
    pr.method = (uint16_t)method;
    pr.flags |= zip_kind(load_le16(r + 4), load_le32(r + 38), &pr.mode);
    if ((gp & 1) || method == 99 || ex.aes) {
        pr.flags |= ARC_F_ENCRYPTED;
        info->encrypted++;
    }
    char buf[1536];
    const char *name;
    size_t nl = zip_name(r + 46, nlen, gp, ex.uni, ex.ulen, buf, sizeof buf, &name);
    int rc = nl == SIZE_MAX ? -ENAMETOOLONG : arc_idx_add(x, name, nl, nullptr, 0, &pr);
    if (rc == -ENOMEM || rc == -EFBIG) return rc;
    if (rc) info->skipped++;
    return 0;
}

static uint64_t item_end(const ArcItem *it, const uint8_t *d)
{
    return it->off + 30 + load_le16(d + it->off + 26) + load_le16(d + it->off + 28) + it->comp_size;
}

/* members whose bytes overlap an earlier one (by offset) are flagged unsafe */
static int zip_overlaps(ArcIndex *x, const uint8_t *d, ArcParseInfo *info)
{
    uint32_t n = x->count;
    if (n < 2) return 0;
    uint32_t *a = mem_alloc((size_t)n * sizeof *a), *b = mem_alloc((size_t)n * sizeof *b);
    if (!a || !b) {
        mem_free(a);
        mem_free(b);
        return -ENOMEM;
    }
    bool sorted = true;
    for (uint32_t i = 0; i < n; i++) {
        a[i] = i;
        if (i && x->items[i - 1].off > x->items[i].off) sorted = false;
    }
    for (uint32_t w = 1; !sorted && w < n; w <<= 1) {
        for (uint32_t lo = 0; lo < n; lo += 2 * w) {
            uint32_t mid = lo + w < n ? lo + w : n, hi = lo + 2 * w < n ? lo + 2 * w : n, i = lo, j = mid, k = lo;
            while (i < mid && j < hi) b[k++] = x->items[a[j]].off < x->items[a[i]].off ? a[j++] : a[i++];
            while (i < mid) b[k++] = a[i++];
            while (j < hi) b[k++] = a[j++];
        }
        uint32_t *t = a;
        a = b;
        b = t;
    }
    uint64_t prev_end = 0;
    for (uint32_t i = 0; i < n; i++) {
        ArcItem *it = &x->items[a[i]];
        if (it->off < prev_end) {
            if (!(it->flags & ARC_F_UNSAFE)) x->unsafe++;
            it->flags |= ARC_F_UNSAFE;
            info->overlap++;
        }
        uint64_t e = item_end(it, d);
        if (e > prev_end) prev_end = e;
    }
    mem_free(a);
    mem_free(b);
    return 0;
}

int arc_zip_parse(ArcIndex *x, const uint8_t *data, uint64_t len, ArcParseInfo *info)
{
    mem_zero(info, sizeof *info);
    int64_t eocd = find_eocd(data, len);
    if (eocd < 0) return (int)eocd;
    ZipDir z;
    int r = zip_dir(data, len, (uint64_t)eocd, &z);
    if (r) return r;
    uint64_t pos = z.start, parsed = 0;
    while (parsed < z.total && z.limit - pos >= 46 && load_le32(data + pos) == SIG_CD) {
        uint64_t used = 0;
        r = zip_record(x, data, len, data + pos, z.limit - pos, z.shift, info, &used);
        if (r < 0) return r;
        if (r > 0) break;
        pos += used;
        parsed++;
    }
    if (parsed < z.total) info->damaged = 1;
    info->end = pos;
    return zip_overlaps(x, data, info);
}
