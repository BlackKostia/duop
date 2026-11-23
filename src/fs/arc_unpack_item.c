/* src/fs/arc_unpack_item.c - creating one archive member below the destination (freestanding C23): folders, files, symlinks.
 *
 * A file is streamed into a temporary sibling (".duop-part-<pid>-<n>", O_EXCL | O_NOFOLLOW), verified by the extractor (CRC-32 and size),
 * stamped with its modification time, flushed when large, and only then renamed to its name - never over a name the user was not asked
 * about (RENAME_NOREPLACE; an existing target goes through the conflict policy). So the real name never holds partial data, whatever
 * kills the process. Trouble with one member (damaged data, encrypted, unsupported method, an unusable name, a clash of kinds) skips
 * that member and goes on; trouble with the destination (full disk, no permission, I/O error) or a cancel stops the run.
 * Symlink targets are checked lexically (relative, never above the destination); a hard link is a copy of its target member. */
#include "duop_arcunpack_int.h"
#include "duop_pal.h"
#include "duop_string.h"

enum { EIO_ = 5, EMFILE_ = 24, EFBIG_ = 27, ENOSPC_ = 28, EROFS_ = 30, EDQUOT_ = 122 };
#define UTIME_OMIT_ ((1 << 30) - 2)
#define FSYNC_MIN (1u << 20)                /* files from this size are flushed before they get their name (many small files: too slow) */

intptr_t arc_xfer_progress(void *ctx, uint64_t n)
{
    (void)ctx;
    fs_progress_bytes(n);
    return fs_progress_cancelled() ? -ECANCELED : 0;
}

/* an error that makes going on pointless: the destination, not the member, is the problem */
static bool fatal(int64_t e)
{
    return e == -ECANCELED || e == -ENOMEM || e == -EIO_ || e == -ENOSPC_ || e == -EDQUOT_ || e == -EROFS_ || e == -EFBIG_ || e == -EACCES || e == -EMFILE_;
}

/* one member left out: counted, its error remembered, the run goes on */
static int note(ArcUnp *u, int64_t e)
{
    u->r->skipped++;
    if (!u->r->first_error) u->r->first_error = e;
    return 0;
}

/* the outcome of a system call on the destination: a fatal error stops the run, any other leaves just this member out */
static int skip(ArcUnp *u, int64_t e) { return fatal(e) ? (int)e : note(u, e); }

static void stamp(int dfd, const char *name, int64_t mtime, int flags)
{
    if (mtime <= 0) return;
    int64_t ts[4] = {0, UTIME_OMIT_, mtime, 0};                               /* the access time stays */
    (void)os_utimensat(dfd, name, ts, flags);
}

/* what to do with an existing target: CONFLICT_ACT_* and, for RENAME, the new name in out (PATH_MAX); -ECANCELED */
static int64_t conflict(ArcUnp *u, int pfd, const char *leaf, uint64_t src_sz, uint64_t dst_sz, char *out)
{
    uint32_t pol = u->o->on_conflict;
    if (!pol) return fs_task_resolve_conflict(pfd, leaf, src_sz, dst_sz, out);
    if (pol == CONFLICT_ACT_RENAME) task_make_unique_name(pfd, leaf, out);
    return pol;
}

static void part_name(ArcUnp *u, char *out)
{
    char *p = out + str_copy(out, ".duop-part-", 48);
    p += u64_to_dec((uint64_t)os_getpid(), p);
    *p++ = '-';
    p += u64_to_dec(u->seq++, p);
    *p = 0;
}

/* the data of `src` as a file named leaf (the metadata of `meta`): after the conflict policy, via a verified temporary file */
static int put_file(ArcUnp *u, const ArcItem *src, const ArcItem *meta, int pfd, const char *leaf)
{
    char renamed[PATH_MAX];
    const char *final = leaf;
    bool replace = false;
    FileStat st;
    if (os_statat(pfd, leaf, &st, 0) == 0) {
        if ((st.mode & S_IFMT) == S_IFDIR) return skip(u, -EISDIR);              /* a folder where a file belongs: never replaced */
        int64_t act = conflict(u, pfd, leaf, src->size, (uint64_t)st.size, renamed);
        if (act < 0) return (int)act;
        if (act == CONFLICT_ACT_SKIP) {
            u->r->kept++;
            return 0;
        }
        if (act == CONFLICT_ACT_RENAME) final = renamed;
        else replace = true;
    }
    char part[64];
    part_name(u, part);
    uint32_t mode = meta->mode ? meta->mode & 0777 : 0644;                   /* no setuid / setgid / sticky; the umask applies */
    intptr_t fd = os_openat(pfd, part, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, mode);
    if (os_is_err(fd)) return skip(u, fd);
    ArcXfer x = {arc_xfer_progress, nullptr, nullptr, 0, 0, 0};
    intptr_t rc = arc_extract_to(&u->src, src, (int)fd, &x);
    if (rc == 0 && src->size >= FSYNC_MIN && os_fsync((int)fd) < 0) rc = x.sink_err = -EIO_;
    if (rc == 0) stamp((int)fd, nullptr, meta->mtime, 0);                      /* futimens before the rename: no race on the name */
    intptr_t cl = os_close((int)fd);
    if (rc == 0 && cl < 0) rc = x.sink_err = cl;
    if (rc == 0) {
        rc = os_renameat2(pfd, part, pfd, final, replace ? 0 : RENAME_NOREPLACE);
        if (rc == -EEXIST) {                                                   /* it appeared meanwhile: leave it alone */
            u->r->kept++;
            rc = 0;
            (void)os_unlinkat(pfd, part, 0);
            return 0;
        }
        if (rc == -EINVAL) rc = os_renameat2(pfd, part, pfd, final, 0);        /* a file system without RENAME_NOREPLACE */
        x.sink_err = rc < 0 ? rc : 0;
    }
    if (rc != 0) {
        (void)os_unlinkat(pfd, part, 0);
        return x.sink_err || !fatal(rc) ? skip(u, x.sink_err ? x.sink_err : rc) : note(u, rc);   /* a failing destination is fatal, bad data only skips */
    }
    u->r->files++;
    u->r->bytes += src->size;
    return 0;
}

static int put_dir(ArcUnp *u, const ArcItem *it, int pfd, const char *leaf)
{
    uint32_t mode = ((it->mode ? it->mode : 0755) | 0700) & 0777;
    intptr_t r = os_mkdirat(pfd, leaf, mode);
    if (r == -EEXIST) {
        FileStat st;
        if (os_statat(pfd, leaf, &st, 0) < 0 || (st.mode & S_IFMT) != S_IFDIR) return skip(u, -EEXIST);   /* a file or link is in the way */
    } else if (r < 0) {
        return skip(u, r);
    } else {
        u->r->dirs++;
    }
    if (u->ndirs == u->cap_dirs) {
        uint32_t cap = u->cap_dirs ? u->cap_dirs * 2 : 64;
        uint32_t *p = mem_realloc(u->dirs, (size_t)cap * sizeof *p);
        if (!p) return -ENOMEM;
        u->dirs = p;
        u->cap_dirs = cap;
    }
    u->dirs[u->ndirs++] = (uint32_t)(it - u->idx.items);
    return 0;
}

/* the link target and a name for Rename: both PATH_MAX, so on the heap */
static int put_symlink_at(ArcUnp *u, const ArcItem *it, const char *rel, int pfd, const char *leaf, char *target, char *renamed)
{
    const char *l = arc_item_link(&u->idx, it);
    if (l) {
        (void)str_copy(target, l, PATH_MAX);
    } else {                                                                   /* zip: the target is the member's data */
        ArcXfer x = {nullptr, nullptr, (uint8_t *)target, PATH_MAX - 1, 0, 0};
        intptr_t rc = arc_extract_to(&u->src, it, -1, &x);
        if (rc) return x.sink_err ? skip(u, rc) : note(u, rc);
        target[x.len] = 0;
    }
    if (!arc_link_safe(rel, target)) return note(u, -EPERM);
    FileStat st;
    if (os_statat(pfd, leaf, &st, 0) == 0) {
        if ((st.mode & S_IFMT) == S_IFDIR) return skip(u, -EISDIR);
        int64_t act = conflict(u, pfd, leaf, 0, (uint64_t)st.size, renamed);
        if (act < 0) return (int)act;
        if (act == CONFLICT_ACT_SKIP) {
            u->r->kept++;
            return 0;
        }
        if (act == CONFLICT_ACT_RENAME) leaf = renamed;
        else (void)os_unlinkat(pfd, leaf, 0);
    }
    intptr_t r = os_symlinkat(target, pfd, leaf);
    if (r < 0) return skip(u, r);
    stamp(pfd, leaf, it->mtime, AT_SYMLINK_NOFOLLOW);
    u->r->links++;
    return 0;
}

static int put_symlink(ArcUnp *u, const ArcItem *it, const char *rel, int pfd, const char *leaf)
{
    char *buf = mem_alloc(2 * PATH_MAX);
    if (!buf) return -ENOMEM;
    int rc = put_symlink_at(u, it, rel, pfd, leaf, buf, buf + PATH_MAX);
    mem_free(buf);
    return rc;
}

int arc_put_member(ArcUnp *u, const ArcItem *it, const char *rel)
{
    if (it->flags & (ARC_F_UNSAFE | ARC_F_SPECIAL)) return note(u, -EPERM);
    if (it->flags & ARC_F_ENCRYPTED) return note(u, -EACCES);
    const char *leaf;
    intptr_t pfd = arc_dest_parent(u->root, rel, true, &leaf);
    if (pfd < 0) return skip(u, pfd);
    int rc;
    if (it->flags & ARC_F_DIR) {
        rc = put_dir(u, it, (int)pfd, leaf);
    } else if (it->flags & ARC_F_SYMLINK) {
        rc = put_symlink(u, it, rel, (int)pfd, leaf);
    } else if (it->flags & ARC_F_HARDLINK) {                                   /* a copy of the target member, never a link to the file system */
        const ArcItem *t = it;
        for (int hops = 0; t && (t->flags & ARC_F_HARDLINK) && hops < 8; hops++) {
            const char *l = arc_item_link(&u->idx, t);
            t = l ? arc_idx_find_raw(&u->idx, l, str_len(l)) : nullptr;
        }
        bool plain = t && !(t->flags & (ARC_F_HARDLINK | ARC_F_SYMLINK | ARC_F_DIR | ARC_F_SPECIAL | ARC_F_UNSAFE));
        rc = plain ? put_file(u, t, it, (int)pfd, leaf) : note(u, -ENOENT);
    } else {
        rc = put_file(u, it, it, (int)pfd, leaf);
    }
    (void)os_close((int)pfd);
    return rc;
}

/* the folders made get their archived times last, deepest first: creating members inside them changed them */
void arc_put_dir_times(ArcUnp *u)
{
    for (uint32_t i = u->ndirs; i-- > 0;) {
        const ArcItem *it = &u->idx.items[u->dirs[i]];
        const char *name = arc_item_name(&u->idx, it);
        size_t n = str_len(name);
        char rel[PATH_MAX];
        if (n >= sizeof rel || it->mtime <= 0) continue;
        (void)str_copy(rel, name + u->selpfx, sizeof rel);
        size_t m = str_len(rel);
        if (m && rel[m - 1] == '/') rel[m - 1] = 0;
        const char *leaf;
        intptr_t pfd = arc_dest_parent(u->root, rel, false, &leaf);
        if (pfd < 0) continue;
        stamp((int)pfd, leaf, it->mtime, AT_SYMLINK_NOFOLLOW);
        (void)os_close((int)pfd);
    }
}
