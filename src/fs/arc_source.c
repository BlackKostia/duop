/* src/fs/arc_source.c - opening an archive for the virtual folder: a read-only mapping of the file (tar, zip), or of the unpacked data
 * (.tar.gz and a plain .gz: gunzipped once into an unlinked temporary file, so a crash can never leave it behind). A plain .gz is an
 * archive of one member, the file it unpacks to. Freestanding C23. */
#include "duop_arcvfs.h"
#include "duop_inflate.h"
#include "duop_oop.h"
#include "duop_pal.h"
#include "duop_string.h"
#include "duop_sys.h"

static intptr_t temp_write(void *ctx, const uint8_t *buf, size_t len)
{
    intptr_t r = sys_write_all(*(int *)ctx, buf, len);
    return r < 0 ? r : 0;
}

static int map_gunzipped(ArcSource *s, const uint8_t *gz, uint64_t gz_len, uint64_t max)
{
    char path[PATH_MAX];
    intptr_t fd = tmp_open_unique("arc_tar", ".tmp", path, sizeof path);
    if (os_is_err(fd)) return (int)fd;
    (void)os_unlinkat(AT_FDCWD, path, 0);                                     /* it lives as long as its descriptor and mapping */
    int ifd = (int)fd;
    CodecSink sink = {temp_write, &ifd};
    CodecResult res;
    intptr_t r = gzip_decode(gz, gz_len, max, &sink, &res);
    if (r < 0 || (!res.produced && s->type != ARC_GZ)) {                       /* an empty tar is damaged, an empty file is a file */
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

int arc_source_open(ArcSource *s, const char *path, int type) { return arc_source_open_max(s, path, type, ARC_OPEN_TGZ_MAX); }

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
    if (type != ARC_TGZ && type != ARC_GZ) {
        s->data = m;
        s->map = m;
        s->len = s->map_len = (uint64_t)st.size;
        return 0;
    }
    TryFrame f;
    volatile int rc = -EIO;
    if (os_try_begin(&f) == 0) {                                              /* the .gz may be cut short under the mapping */
        if (st.size >= 10) s->mtime = load_le32((const uint8_t *)m + 4);      /* the gzip header time */
        rc = map_gunzipped(s, m, (uint64_t)st.size, max_unpacked);
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

/* the one member of a plain .gz: the file name without .gz (or with ".out" when it has another name), the unpacked bytes in place */
static int gz_member(ArcIndex *x, const ArcSource *s, const char *path)
{
    char name[300];
    const char *base = path_basename(path);
    size_t n = str_len(base);
    if (n > 290) return -ENAMETOOLONG;
    __builtin_memcpy(name, base, n + 1);
    if (n > 3 && !str_casecmp(name + n - 3, ".gz")) name[n - 3] = 0;
    else if (n > 4 && !str_casecmp(name + n - 4, ".tgz")) name[n - 4] = 0;
    else (void)str_cat(name, ".out", sizeof name);
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
    if (s->type == ARC_GZ) return gz_member(x, s, path);
    return arc_tar_parse(x, s->data, s->len, info);
}
