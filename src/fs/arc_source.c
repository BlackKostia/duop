/* src/fs/arc_source.c - opening an archive for the virtual folder: a read-only mapping of the file (tar, zip), or of the unpacked data
 * (.tar.gz / .tar.zst / .tar.xz / .tar.bz2 and a plain .gz / .zst / .xz / .bz2: decompressed once into an unlinked temporary file, so a crash can never leave it behind).
 * A plain .gz / .zst / .xz / .bz2 is an archive of one member, the file it unpacks to. Freestanding C23. */
#include "duop_arcvfs.h"
#include "duop_bzip2.h"
#include "duop_inflate.h"
#include "duop_oop.h"
#include "duop_pal.h"
#include "duop_string.h"
#include "duop_sys.h"
#include "duop_xz.h"
#include "duop_zstd.h"

static intptr_t temp_write(void *ctx, const uint8_t *buf, size_t len)
{
    intptr_t r = sys_write_all(*(int *)ctx, buf, len);
    return r < 0 ? r : 0;
}

typedef intptr_t (*DecodeFn)(const void *src, uint64_t len, uint64_t max_out, const CodecSink *sink, CodecResult *res);

/* the decompressor of a compressed type, NULL for tar and zip */
static DecodeFn decoder_of(int type)
{
    if (type == ARC_TGZ || type == ARC_GZ) return gzip_decode;
    if (type == ARC_TZST || type == ARC_ZST) return zstd_decode;
    if (type == ARC_TXZ || type == ARC_XZ) return xz_decode;
    if (type == ARC_TBZ || type == ARC_BZ) return bzip2_decode;
    return nullptr;
}

static int map_decoded(ArcSource *s, DecodeFn decode, const uint8_t *gz, uint64_t gz_len, uint64_t max)
{
    char path[PATH_MAX];
    intptr_t fd = tmp_open_unique("arc_tar", ".tmp", path, sizeof path);
    if (os_is_err(fd)) return (int)fd;
    (void)os_unlinkat(AT_FDCWD, path, 0);                                     /* it lives as long as its descriptor and mapping */
    int ifd = (int)fd;
    CodecSink sink = {temp_write, &ifd};
    CodecResult res;
    intptr_t r = decode(gz, gz_len, max, &sink, &res);
    if (r < 0 || (!res.produced && s->type != ARC_GZ && s->type != ARC_ZST && s->type != ARC_XZ && s->type != ARC_BZ)) {   /* an empty tar is damaged, an empty file is a file */
        (void)os_close(ifd);
        return r < 0 ? (int)r : -EINVAL;
    }
    if (!res.produced) {
        (void)os_close(ifd);
        return 0;                                                              /* data stays empty: nothing to map */
    }
    void *m = os_mmap_file_ro(ifd, (size_t)res.produced);
    (void)os_close(ifd);
    if (!m) return -ENOMEM;
    s->data = m;
    s->map = m;
    s->len = s->map_len = res.produced;
    return 0;
}

/* the open is synchronous (the pane waits), so a slower decoder may unpack less: bzip2 a quarter of the limit, xz half, the rest all of it */
static uint64_t open_cap(int type)
{
    return type == ARC_TBZ || type == ARC_BZ ? ARC_OPEN_TGZ_MAX >> 2 : type == ARC_TXZ || type == ARC_XZ ? ARC_OPEN_TGZ_MAX >> 1 : ARC_OPEN_TGZ_MAX;
}

int arc_source_open(ArcSource *s, const char *path, int type) { return arc_source_open_max(s, path, type, open_cap(type)); }

int arc_source_open_max(ArcSource *s, const char *path, int type, uint64_t max_unpacked)
{
    mem_zero(s, sizeof *s);
    s->type = type;
    intptr_t fd = os_openat(AT_FDCWD, path, O_RDONLY | O_CLOEXEC | O_NONBLOCK, 0);   /* a FIFO named like an archive must not block */
    if (os_is_err(fd)) return (int)fd;
    FileStat st;
    int r = os_fstat((int)fd, &st);
    if (r < 0 || (st.mode & S_IFMT) != S_IFREG || st.size <= 0) {
        (void)os_close((int)fd);
        return r < 0 ? r : -EINVAL;
    }
    void *m = os_mmap_file_ro((int)fd, (size_t)st.size);
    (void)os_close((int)fd);
    if (!m) return -ENOMEM;
    DecodeFn decode = decoder_of(type);
    if (!decode) {
        s->data = m;
        s->map = m;
        s->len = s->map_len = (uint64_t)st.size;
        return 0;
    }
    TryFrame f;
    volatile int rc = -EIO;
    if (os_try_begin(&f) == 0) {                                              /* the file may be cut short under the mapping */
        if (type == ARC_GZ && st.size >= 10) s->mtime = load_le32((const uint8_t *)m + 4);   /* the gzip header time */
        rc = map_decoded(s, decode, m, (uint64_t)st.size, max_unpacked);
        os_try_end(&f);
    }
    os_munmap(m, (size_t)st.size);
    return rc;
}

void arc_source_close(ArcSource *s)
{
    if (s->map) os_munmap(s->map, (size_t)s->map_len);
    mem_zero(s, sizeof *s);
}

/* the one member of a plain .gz / .zst: the file name without the suffix (or with ".out" when it has another name), the unpacked bytes in place */
static int single_member(ArcIndex *x, const ArcSource *s, const char *path)
{
    static const char *const sfx[] = {".gz", ".zst", ".xz", ".bz2", ".tgz", ".tzst", ".txz", ".tbz2", ".tbz"};
    char name[300];
    const char *base = path_basename(path);
    size_t n = str_len(base);
    if (n > 290) return -ENAMETOOLONG;
    __builtin_memcpy(name, base, n + 1);
    bool cut = false;
    for (size_t i = 0; i < sizeof sfx / sizeof sfx[0] && !cut; i++) {
        size_t k = str_len(sfx[i]);
        if (n > k && !str_casecmp(name + n - k, sfx[i])) {
            name[n - k] = 0;
            cut = true;
        }
    }
    if (!cut) (void)str_cat(name, ".out", sizeof name);
    if (!name[0]) (void)str_copy(name, "data", sizeof name);
    ArcItem proto = {0};
    proto.size = proto.comp_size = s->len;
    proto.mtime = s->mtime;
    proto.mode = 0644;
    return arc_idx_add(x, name, str_len(name), nullptr, 0, &proto);
}

int arc_parse_source(const ArcSource *s, const char *path, ArcIndex *x, ArcParseInfo *info)
{
    if (s->type == ARC_ZIP) return arc_zip_parse(x, s->data, s->len, info);
    if (s->type == ARC_GZ || s->type == ARC_ZST || s->type == ARC_XZ || s->type == ARC_BZ) return single_member(x, s, path);
    return arc_tar_parse(x, s->data, s->len, info);
}
