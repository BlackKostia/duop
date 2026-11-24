/* src/fs/archive_parse.c - the index of a .tar / .tar.gz / .zip held in memory (freestanding C23; replaces the parsers of
 * archive_vfs.s and archive_extract.s, kept as tests/harness/archive_vfs_reference.s and archive_extract_reference.s).
 *
 * The input is untrusted: every offset and length is compared as "is it inside the remaining buffer" without forming sums that
 * could wrap (a ZIP64 directory may claim any 64-bit offset). Entries keep their sizes clamped to the data present. */
#include "duop_arc.h"
#include "duop_inflate.h"
#include "duop_string.h"

static bool arc_ends_ci(const char *s, size_t len, const char *sfx, size_t sfx_len)
{
    return len >= sfx_len && !str_casecmp(s + len - sfx_len, sfx);
}

/* files that are zips under another name (office documents, packages, e-books, ...) */
static const char *const zip_based[] = {".docx", ".xlsx", ".pptx", ".docm", ".xlsm", ".odt", ".ods", ".odp", ".odg", ".epub", ".jar", ".war", ".ear",
                                        ".apk", ".aab", ".aar", ".whl", ".egg", ".xpi", ".crx", ".nupkg", ".vsix", ".cbz"};
/* formats that are known but not implemented yet (xz / bzip2 / zstd / 7z / rar ...) */
static const char *const not_yet[] = {".7z", ".rar", ".bz2", ".tbz", ".tbz2", ".lz4", ".lzma", ".lz", ".cab"};

static bool arc_in_list(const char *name, size_t n, const char *const *list, size_t count)
{
    for (size_t i = 0; i < count; i++)
        if (arc_ends_ci(name, n, list[i], str_len(list[i]))) return true;
    return false;
}

int arc_is_archive(const char *name)
{
    size_t n = str_len(name);
    if (arc_ends_ci(name, n, ".tar.gz", 7) || arc_ends_ci(name, n, ".tgz", 4)) return ARC_TGZ;
    if (arc_ends_ci(name, n, ".tar", 4)) return ARC_TAR;
    if (arc_ends_ci(name, n, ".zip", 4) || arc_in_list(name, n, zip_based, sizeof zip_based / sizeof zip_based[0])) return ARC_ZIP;
    return 0;
}

bool arc_unsupported_format(const char *name)
{
    return !arc_is_archive(name) && arc_in_list(name, str_len(name), not_yet, sizeof not_yet / sizeof not_yet[0]);
}

/* tar numbers: octal digits, blanks skipped, ended by NUL or anything else */
static uint64_t arc_octal(const uint8_t *p, int n)
{
    uint64_t v = 0;
    for (int i = 0; i < n && p[i]; i++) {
        if (p[i] == ' ') continue;
        if (p[i] < '0' || p[i] > '7') break;
        v = v << 3 | (uint64_t)(p[i] - '0');
    }
    return v;
}

void arc_parse_tar(ArcCtx *ctx)
{
    const uint8_t *raw = ctx->raw;
    uint64_t len = ctx->raw_len, off = 0;
    while (len - off >= 512 && raw[off] && ctx->count < ARC_MAX_ENTRIES) {   /* off <= len holds throughout */
        ArcEntry *e = &ctx->entries[ctx->count];
        (void)str_copy(e->name, (const char *)raw + off, 100);
        uint64_t data = off + 512, avail = len - data, size = arc_octal(raw + off + 124, 12);
        if (size > avail) size = avail;                                  /* clamp to the buffer */
        e->size = e->comp_size = size;
        e->off = data;
        e->method = 0;
        e->is_dir = raw[off + 156] == '5';
        ctx->count++;
        off = data + ((size + 511) & ~(uint64_t)511);
        if (off > len) break;
    }
}

/* the ZIP64 end-of-central-directory record behind its locator: (cd offset, entries) */
static void arc_zip64_eocd(const uint8_t *raw, uint64_t len, uint64_t eocd, uint64_t *cd, uint64_t *total)
{
    if (eocd < 20 || load_le32(raw + eocd - 20) != 0x07064b50) return;
    uint64_t z = load_le64(raw + eocd - 12);
    if (z > len || len - z < 56 || load_le32(raw + z) != 0x06064b50) return;
    *total = load_le64(raw + z + 32);
    if (*total > ARC_MAX_ENTRIES) *total = ARC_MAX_ENTRIES;
    *cd = load_le64(raw + z + 48);
}

/* the ZIP64 extra field (0x0001) of a central record: the 64-bit size / compressed size / offset that read 0xFFFFFFFF */
static void arc_zip64_extra(ArcEntry *e, const uint8_t *p, uint64_t rem)
{
    const uint64_t big = 0xFFFFFFFFu;
    if (e->size != big && e->comp_size != big && e->off != big) return;
    while (rem >= 4) {
        uint32_t id = load_le16(p), sz = load_le16(p + 2);
        p += 4;
        rem -= 4;
        if (sz > rem) return;
        if (id != 1) { p += sz; rem -= sz; continue; }
        uint64_t *fields[3] = {&e->size, &e->comp_size, &e->off};
        for (int i = 0; i < 3; i++) {
            if (*fields[i] != big) continue;
            if (sz < 8) return;
            *fields[i] = load_le64(p);
            p += 8;
            sz -= 8;
        }
        return;
    }
}

/* the local header must lie in the buffer; sizes are clamped to the data behind it. Only then the entry counts. */
static void arc_zip_validate(ArcCtx *ctx, ArcEntry *e)
{
    uint64_t len = ctx->raw_len, off = e->off;
    if (off > len || len - off < 30) return;
    uint64_t hdr = 30 + (uint64_t)load_le16(ctx->raw + off + 26) + load_le16(ctx->raw + off + 28);
    if (hdr > len - off) return;
    uint64_t avail = len - off - hdr;
    if (e->comp_size > avail) e->comp_size = avail;
    if (!e->method && e->size > e->comp_size) e->size = e->comp_size;
    ctx->count++;
}

void arc_parse_zip(ArcCtx *ctx)
{
    const uint8_t *raw = ctx->raw;
    uint64_t len = ctx->raw_len;
    if (len < 22) return;
    uint64_t eocd = len - 22;
    for (int steps = 0; load_le32(raw + eocd) != 0x06054b50; steps++) {   /* behind a comment of up to 64 KiB */
        if (!eocd || steps + 1 >= 65536) return;
        eocd--;
    }
    uint64_t cd = load_le32(raw + eocd + 16), total = load_le16(raw + eocd + 10);
    if (cd == 0xFFFFFFFFu || total == 0xFFFF) arc_zip64_eocd(raw, len, eocd, &cd, &total);
    for (uint32_t i = 0; i < (uint32_t)total; i++) {
        if (cd > len || len - cd < 46 || load_le32(raw + cd) != 0x02014b50 || ctx->count >= ARC_MAX_ENTRIES) return;
        const uint8_t *r = raw + cd;
        ArcEntry *e = &ctx->entries[ctx->count];
        e->method = load_le16(r + 10);
        e->comp_size = load_le32(r + 20);
        e->size = load_le32(r + 24);
        e->off = load_le32(r + 42);
        uint64_t nlen = load_le16(r + 28), xlen = load_le16(r + 30), clen = load_le16(r + 32);
        if (nlen > len - cd - 46) return;
        (void)str_copy(e->name, (const char *)r + 46, (nlen > 255 ? 255 : nlen) + 1);
        size_t n = str_len(e->name);
        e->is_dir = n && e->name[n - 1] == '/';
        if (xlen <= len - cd - 46 - nlen) arc_zip64_extra(e, r + 46 + nlen, xlen);
        arc_zip_validate(ctx, e);
        cd += 46 + nlen + xlen + clen;
    }
}

/* gzip member -> inflated data (capacity: ISIZE clamped to 64 KiB .. 8 MiB) */
void *arc_ungzip(const uint8_t *src, uint64_t len, uint64_t *out_len)
{
    if (len < 18 || load_le16(src) != 0x8B1F || src[2] != 8) return nullptr;
    uint8_t flg = src[3];
    uint64_t p = 10;
    if (flg & 4) {                                                      /* FEXTRA */
        if (len - p < 2) return nullptr;
        p += 2 + (uint64_t)load_le16(src + p);
    }
    for (int f = 8; f <= 16; f += 8)                                     /* FNAME, FCOMMENT: NUL-terminated */
        if (flg & f) {
            do {
                if (p >= len) return nullptr;
            } while (src[p++]);
        }
    if (flg & 2) p += 2;                                                 /* FHCRC */
    if (p > len || len - p < 8) return nullptr;
    uint32_t cap = load_le32(src + len - 4);                             /* ISIZE */
    if (cap < 65536) cap = 65536;
    if (cap > 8388608) cap = 8388608;
    uint8_t *dst = mem_alloc(cap);
    if (!dst) return nullptr;
    intptr_t r = inflate_raw(src + p, len - p - 8, dst, cap);
    if (r < 0) {
        mem_free(dst);
        return nullptr;
    }
    *out_len = (uint64_t)r;
    return dst;
}
