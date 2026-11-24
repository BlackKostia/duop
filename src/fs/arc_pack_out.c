/* src/fs/arc_pack_out.c - where a pack run writes (freestanding C23): the temporary sibling of the destination, the gzip layer of .tar.gz, the
 * commit by rename without replacing anything, and the clean-up of everything on failure. */
#include "duop_arcpack_int.h"
#include "duop_fs.h"
#include "duop_hash.h"
#include "duop_oop.h"
#include "duop_pal.h"
#include "duop_string.h"
#include "duop_sys.h"

static intptr_t file_write(void *ctx, const uint8_t *p, size_t n)
{
    PackOut *o = ctx;
    if (fs_progress_cancelled()) return o->err = -ECANCELED;
    intptr_t r = sys_write_all(o->fd, p, n);
    if (r < 0) return o->err = (int)r;
    o->bytes_out += n;
    return 0;
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static intptr_t gz_write(void *ctx, const uint8_t *p, size_t n)
{
    PackOut *o = ctx;
    o->crc = crc32_update(o->crc, p, n);
    o->usize += n;
    int r = deflate_write(o->def, p, n);
    return r ? r : 0;
}

/* the 10-byte gzip header: deflate, no flags, no time (reproducible), the level hint, Unix */
static int gz_header(PackOut *o, uint32_t level)
{
    uint8_t h[10] = {0x1F, 0x8B, 8, 0, 0, 0, 0, 0, (uint8_t)(level >= 9 ? 2 : level == 1 ? 4 : 0), 3};
    return (int)o->file.write(o, h, sizeof h);
}

int pack_out_open(PackOut *o, const char *dst, uint32_t format, uint32_t level)
{
    mem_zero(o, sizeof *o);
    o->fd = -1;
    FileStat st;
    if (os_statat(AT_FDCWD, dst, &st, 0) == 0) return -EEXIST;
    if (str_len(dst) + 32 >= PATH_MAX) return -ENAMETOOLONG;
    str_copy(o->dst, dst, PATH_MAX);
    char dir[PATH_MAX];
    path_dirname(dst, dir, sizeof dir);
    for (uint32_t i = 0; i < 100; i++) {                           /* ".<name>.duop-part-<n>": the first that is free */
        char suffix[24] = ".duop-part-";
        suffix[11] = (char)('0' + i / 10);
        suffix[12] = (char)('0' + i % 10);
        suffix[13] = 0;
        char leaf[300] = ".";
        str_cat(leaf, path_basename(dst), sizeof leaf);
        str_cat(leaf, suffix, sizeof leaf);
        if (path_join(o->part, dir, leaf, PATH_MAX) < 0) return -ENAMETOOLONG;
        intptr_t fd = os_openat(AT_FDCWD, o->part, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
        if (fd == -EEXIST) continue;
        if (os_is_err(fd)) return (int)fd;
        o->fd = (int)fd;
        break;
    }
    if (o->fd < 0) return -EEXIST;
    if (os_fstat(o->fd, &st) == 0) {
        o->dev = st.dev;
        o->ino = st.ino;
    }
    o->file = (CodecSink){file_write, o};
    o->top = o->file;
    if (format != ARC_PACK_TGZ) return 0;
    o->crc = 0xFFFFFFFFu;
    o->gz_out = o->file;
    o->def = deflate_new((int)level, &o->gz_out);
    if (!o->def) return -ENOMEM;
    o->gz_in = (CodecSink){gz_write, o};
    o->top = o->gz_in;
    return gz_header(o, level);
}

int pack_out_close(PackOut *o, bool commit)
{
    int r = 0;
    if (commit && o->def) {                                        /* the gzip trailer: CRC-32 and size mod 2^32 */
        r = deflate_finish(o->def);
        uint8_t t[8];
        put32(t, ~o->crc);
        put32(t + 4, (uint32_t)o->usize);
        if (!r) r = (int)o->file.write(o, t, sizeof t);
    }
    deflate_free(o->def);
    o->def = nullptr;
    if (commit && !r && os_fsync(o->fd) < 0) r = -EIO;
    if (o->fd >= 0 && os_close(o->fd) < 0 && !r && commit) r = -EIO;
    o->fd = -1;
    if (commit && !r) {
        intptr_t m = os_renameat2(AT_FDCWD, o->part, AT_FDCWD, o->dst, RENAME_NOREPLACE);
        if (m == -EINVAL) {                                         /* a file system without RENAME_NOREPLACE: look first */
            FileStat st;
            m = os_statat(AT_FDCWD, o->dst, &st, 0) == 0 ? -EEXIST : os_renameat2(AT_FDCWD, o->part, AT_FDCWD, o->dst, 0);
        }
        r = (int)m;
    }
    if (r || !commit) (void)os_unlinkat(AT_FDCWD, o->part, 0);
    return r;
}
