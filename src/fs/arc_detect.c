/* src/fs/arc_detect.c - what an archive file is, decided by its contents (freestanding C23).
 *
 * A file is read for at most one KiB (and, for gzip, zstd, xz and bzip2, enough input to see the first 512 bytes of what it unpacks to): zip, gzip,
 * zstd, xz and bzip2 by their magic, a tar by a valid header checksum, a gzip / zstd file as a tar.gz / tar.zst when it unpacks to a tar and as a
 * one-member archive otherwise. Formats that are known but not implemented yet (7z, rar, ...) are recognised by magic too,
 * so that Enter says why instead of opening a hex dump. Where the contents say nothing the extension decides (self-extracting zips, v7 tars,
 * unreadable files), so a file that was an archive by name stays one. Only regular files are opened, without blocking.
 * Text that merely looks like a magic is not mistaken for one: bzip2 needs its block signature, zip its record signature. */
#include "duop_arcvfs.h"
#include "duop_bzip2.h"
#include "duop_inflate.h"
#include "duop_pal.h"
#include "duop_string.h"
#include "duop_sys.h"
#include "duop_xz.h"
#include "duop_zstd.h"

#define HEAD_MIN 1024u                      /* enough for every magic below and for a tar header */
#define GZ_PEEK (96u << 10)                 /* gzip input handed to the decoder: one 64 KiB window of output needs no more */
#define BZ_PEEK ((1u << 20) + (128u << 10))   /* bzip2 decodes a whole block before it outputs a byte: one of at most 900000 symbols, packed */
#define XZ_PEEK (96u << 10)                 /* xz: the headers and the first LZMA2 chunk (at most 64 KiB packed) */
#define ZST_PEEK (144u << 10)               /* zstd: the frame header and the whole first block (at most 128 KiB, stored when it does not compress) */

typedef struct {
    uint8_t blk[512];
    size_t n;
} Peek;

static intptr_t peek_write(void *ctx, const uint8_t *buf, size_t len)
{
    Peek *p = ctx;
    size_t take = 512 - p->n < len ? 512 - p->n : len;
    __builtin_memcpy(p->blk + p->n, buf, take);
    p->n += take;
    return 0;
}

static bool starts(const uint8_t *h, size_t n, const uint8_t *m, size_t k) { return n >= k && !__builtin_memcmp(h, m, k); }

/* magics of formats that are not implemented yet */
static bool known_unsupported(const uint8_t *h, size_t n)
{
    static const struct { uint8_t len; uint8_t m[8]; } tab[] = {
        {6, {'7', 'z', 0xBC, 0xAF, 0x27, 0x1C}},        /* 7z */
        {6, {'R', 'a', 'r', '!', 0x1A, 0x07}},          /* rar */
        {4, {0x04, 0x22, 0x4D, 0x18}},                  /* lz4 */
        {8, {'M', 'S', 'C', 'F', 0, 0, 0, 0}},          /* cab */
    };
    for (size_t i = 0; i < sizeof tab / sizeof tab[0]; i++)
        if (starts(h, n, tab[i].m, tab[i].len)) return true;
    return false;
}

static bool all_zero(const uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (p[i]) return false;
    return true;
}

typedef intptr_t (*DecodeFn)(const void *src, uint64_t len, uint64_t max_out, const CodecSink *sink, CodecResult *res);

/* a gzip / zstd file: a tar.* when its first 512 output bytes are a tar header (a zero block counts when the name says so), else one member */
static int classify_packed(int fd, const uint8_t *head, size_t have, size_t cap, DecodeFn decode, int tar_type, int single_type, int by_ext)
{
    uint8_t *buf = mem_alloc(cap);
    if (!buf) return by_ext == tar_type ? tar_type : single_type;
    __builtin_memcpy(buf, head, have);
    size_t n = have;
    while (n < cap) {
        intptr_t r = sys_read_retry(fd, buf + n, cap - n);
        if (r <= 0) break;
        n += (size_t)r;
    }
    Peek pk = {{0}, 0};
    CodecSink sink = {peek_write, &pk};
    CodecResult res;
    (void)decode(buf, n, 512, &sink, &res);                         /* -EFBIG once the 512 bytes are out: that is the intended stop */
    mem_free(buf);
    if (pk.n == 512 && (arc_tar_checksum_ok(pk.blk) || (by_ext == tar_type && all_zero(pk.blk, 512)))) return tar_type;
    return single_type;                                             /* under 512 bytes it cannot be a tar; a cut stream fails on open either way */
}

static int classify(int fd, const uint8_t *h, size_t n, int by_ext)
{
    static const uint8_t pk34[] = {'P', 'K', 3, 4}, pk56[] = {'P', 'K', 5, 6};
    if (starts(h, n, pk34, 4) || starts(h, n, pk56, 4)) return ARC_ZIP;
    if (n >= 3 && h[0] == 0x1F && h[1] == 0x8B && h[2] == 8) return classify_packed(fd, h, n, GZ_PEEK, gzip_decode, ARC_TGZ, ARC_GZ, by_ext);
    static const uint8_t xz_magic[] = {0xFD, '7', 'z', 'X', 'Z', 0};
    static const uint8_t bz_block[] = {0x31, 0x41, 0x59, 0x26, 0x53, 0x59}, bz_end[] = {0x17, 0x72, 0x45, 0x38, 0x50, 0x90};
    if (n >= 10 && h[0] == 'B' && h[1] == 'Z' && h[2] == 'h' && h[3] >= '1' && h[3] <= '9' && (starts(h + 4, n - 4, bz_block, 6) || starts(h + 4, n - 4, bz_end, 6)))
        return classify_packed(fd, h, n, BZ_PEEK, bzip2_decode, ARC_TBZ, ARC_BZ, by_ext);                  /* a block, or the end of an empty stream */
    if (starts(h, n, xz_magic, 6)) return classify_packed(fd, h, n, XZ_PEEK, xz_decode, ARC_TXZ, ARC_XZ, by_ext);
    if (n >= 4 && ((h[0] == 0x28 && h[1] == 0xB5 && h[2] == 0x2F && h[3] == 0xFD) || ((h[0] & 0xF0) == 0x50 && h[1] == 0x2A && h[2] == 0x4D && h[3] == 0x18)))
        return classify_packed(fd, h, n, ZST_PEEK, zstd_decode, ARC_TZST, ARC_ZST, by_ext);       /* a frame, or a skippable frame first */
    if (known_unsupported(h, n)) return ARC_UNSUPPORTED;
    if (n >= 512 && h[0] && arc_tar_checksum_ok(h)) return ARC_TAR;
    return -1;                                                      /* the contents say nothing: the extension decides */
}

static bool ends_ci(const char *s, const char *sfx)
{
    size_t n = str_len(s), k = str_len(sfx);
    return n >= k && !str_casecmp(s + n - k, sfx);
}

/* the type the name says: arc_is_archive and the Zstandard names (kept out of arc_is_archive: the package manager does not read them) */
static int ext_type(const char *path)
{
    int t = arc_is_archive(path);
    if (t) return t;
    if (ends_ci(path, ".tar.zst") || ends_ci(path, ".tzst")) return ARC_TZST;
    if (ends_ci(path, ".tar.xz") || ends_ci(path, ".txz")) return ARC_TXZ;
    if (ends_ci(path, ".tar.bz2") || ends_ci(path, ".tbz2") || ends_ci(path, ".tbz")) return ARC_TBZ;
    if (ends_ci(path, ".bz2")) return ARC_BZ;
    if (ends_ci(path, ".zst")) return ARC_ZST;
    return ends_ci(path, ".xz") ? ARC_XZ : 0;
}

int arc_detect_file(const char *path)
{
    int by_ext = ext_type(path);
    int fallback = by_ext ? by_ext : arc_unsupported_format(path) ? ARC_UNSUPPORTED : 0;
    intptr_t fd = os_openat(AT_FDCWD, path, O_RDONLY | O_CLOEXEC | O_NONBLOCK, 0);   /* a FIFO must not block the pane */
    if (os_is_err(fd)) return fallback;
    uint8_t head[HEAD_MIN];
    FileStat st;
    int kind = -1;
    if (os_fstat((int)fd, &st) == 0 && (st.mode & S_IFMT) == S_IFREG && st.size > 0) {
        intptr_t n = sys_read_retry((int)fd, head, sizeof head);
        if (n > 0) kind = classify((int)fd, head, (size_t)n, by_ext);
    }
    (void)os_close((int)fd);
    return kind < 0 ? fallback : kind;
}
