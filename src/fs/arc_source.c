/* src/fs/arc_source.c - opening an archive for the virtual folder: a read-only mapping of the file (tar, zip), or of the unpacked tar
 * (.tar.gz: gunzipped once into an unlinked temporary file, so a crash can never leave it behind). Freestanding C23. */
#include "duop_arcvfs.h"
#include "duop_inflate.h"
#include "duop_oop.h"
#include "duop_pal.h"
#include "duop_sys.h"

static intptr_t temp_write(void *ctx, const uint8_t *buf, size_t len)
{
    intptr_t r = sys_write_all(*(int *)ctx, buf, len);
    return r < 0 ? r : 0;
}

static int map_gunzipped(ArcSource *s, const uint8_t *gz, uint64_t gz_len)
{
    char path[PATH_MAX];
    intptr_t fd = tmp_open_unique("arc_tar", ".tmp", path, sizeof path);
    if (os_is_err(fd)) return (int)fd;
    (void)os_unlinkat(AT_FDCWD, path, 0);                                     /* it lives as long as its descriptor and mapping */
    int ifd = (int)fd;
    CodecSink sink = {temp_write, &ifd};
    CodecResult res;
    intptr_t r = gzip_decode(gz, gz_len, ARC_OPEN_TGZ_MAX, &sink, &res);
    if (r < 0 || !res.produced) {
        (void)os_close(ifd);
        return r < 0 ? (int)r : -EINVAL;
    }
    void *m = os_mmap_file_ro(ifd, (size_t)res.produced);
    (void)os_close(ifd);
    if (!m) return -ENOMEM;
    s->data = m;
    s->map = m;
    s->len = s->map_len = res.produced;
    return 0;
}

int arc_source_open(ArcSource *s, const char *path, int type)
{
    mem_zero(s, sizeof *s);
    s->type = type;
    intptr_t fd = os_openat(AT_FDCWD, path, O_RDONLY | O_CLOEXEC, 0);
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
    if (type != ARC_TGZ) {
        s->data = m;
        s->map = m;
        s->len = s->map_len = (uint64_t)st.size;
        return 0;
    }
    TryFrame f;
    volatile int rc = -EIO;
    if (os_try_begin(&f) == 0) {                                              /* the .gz may be cut short under the mapping */
        rc = map_gunzipped(s, m, (uint64_t)st.size);
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
