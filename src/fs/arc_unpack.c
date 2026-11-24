/* src/fs/arc_unpack.c - unpacking and testing an archive (freestanding C23): open and index it, select the members, check the limits,
 * create every member below the destination (arc_unpack_item.c), report. Runs inside the TaskWorker, which publishes the totals and the
 * progress into the shared page (the hooks are no-ops elsewhere).
 *
 * Order of events: index -> selection (de-duplicated, in name order) -> totals -> refuse a byte cap / decompression bomb -> open (or create)
 * the destination -> free-space check -> members -> folder times. Nothing is written before the checks pass. */
#include "duop_arcunpack_int.h"
#include "duop_pal.h"
#include "duop_string.h"
#include "duop_sys.h"

enum { ENOSPC_ = 28 };
#define TGZ_UNPACK_MAX (4ull << 30)          /* the worker may gunzip a .tar.gz up to this size into the temporary file */

static int open_index(ArcUnp *u, const char *path, ArcUnpackReport *r)
{
    int type = arc_detect_file(path);
    if (type == ARC_UNSUPPORTED) return -EOPNOTSUPP;
    if (type <= 0) return -EINVAL;
    int rc = arc_source_open_max(&u->src, path, type, TGZ_UNPACK_MAX);
    if (rc) return rc;
    arc_idx_init(&u->idx);
    TryFrame f;
    volatile int pr = -EIO;
    if (os_try_begin(&f) == 0) {                                               /* the archive may be cut short under its mapping */
        pr = arc_parse_source(&u->src, path, &u->idx, &u->info);
        os_try_end(&f);
    }
    if (pr == 0) pr = arc_idx_finish(&u->idx);
    r->damaged = u->info.damaged;
    return pr;
}

static void close_all(ArcUnp *u)
{
    if (u->root >= 0) (void)os_close(u->root);
    mem_free(u->dirs);
    arc_idx_free(&u->idx);
    arc_source_close(&u->src);
}

/* ---- the selection ---- */
typedef struct { uint32_t *sel; uint32_t n; uint8_t *done; uint64_t bytes; } Sel;

static int sel_add(ArcUnp *u, Sel *s, uint32_t item)
{
    if (s->done[item >> 3] & (1u << (item & 7))) return 0;                    /* chosen twice (overlapping selections) */
    s->done[item >> 3] |= (uint8_t)(1u << (item & 7));
    s->sel[s->n++] = item;
    const ArcItem *it = &u->idx.items[item];
    if (!(it->flags & (ARC_F_DIR | ARC_F_UNSAFE | ARC_F_SPECIAL | ARC_F_SYMLINK)) && !(it->flags & ARC_F_HARDLINK)) s->bytes += it->size;
    return 0;
}

/* the members named (or everything): `names` = "<folder>\0<member>\0..." */
static int select_items(ArcUnp *u, Sel *s, const char *names, uint32_t count)
{
    ArcIndex *x = &u->idx;
    s->sel = mem_alloc(((size_t)x->unique + 1) * sizeof *s->sel);
    s->done = mem_alloc((size_t)x->count / 8 + 1);
    char *key = mem_alloc(PATH_MAX);
    if (!s->sel || !s->done || !key) {
        mem_free(key);
        return -ENOMEM;
    }
    mem_zero(s->done, (size_t)x->count / 8 + 1);
    u->selpfx = str_len(names);
    if (!count) {
        for (uint32_t i = 0; i < x->unique; i++) (void)sel_add(u, s, x->order[i]);
    } else {
        const char *p = names + u->selpfx + 1;
        for (uint32_t k = 0; k < count; k++, p += str_len(p) + 1) {
            if (path_join(key, names, p, PATH_MAX) < 0) continue;              /* sub + member (sub ends in '/') */
            const ArcItem *it = arc_idx_find(x, key);
            if (it) (void)sel_add(u, s, (uint32_t)(it - x->items));
            size_t n = str_len(key);
            if (n + 2 >= PATH_MAX) continue;
            key[n] = '/';
            key[n + 1] = 0;                                                    /* the folder's subtree, its own entry first */
            for (uint32_t i = arc_idx_lower(x, key); i < x->unique && str_starts_with(arc_item_name(x, &x->items[x->order[i]]), key); i++)
                (void)sel_add(u, s, x->order[i]);
        }
    }
    mem_free(key);
    return 0;
}

/* the destination directory: opened, or made (one level) when missing */
static intptr_t open_root(const char *dst)
{
    intptr_t fd = os_openat(AT_FDCWD, dst, O_RDONLY | O_DIRECTORY | O_CLOEXEC, 0);
    if (fd != -ENOENT) return fd;
    intptr_t m = os_mkdirat(AT_FDCWD, dst, 0755);
    if (m < 0 && m != -EEXIST) return m;
    return os_openat(AT_FDCWD, dst, O_RDONLY | O_DIRECTORY | O_CLOEXEC, 0);
}

/* "<name>" of a path "/x/name.tar.gz" without its archive extension */
static void archive_stem(const char *path, char *out)
{
    (void)str_copy(out, path_basename(path), 256);
    static const char *const ext[] = {".tar.gz", ".tgz", ".tar", ".zip"};
    size_t n = str_len(out);
    for (int i = 0; i < 4; i++) {
        size_t e = str_len(ext[i]);
        if (n > e && !str_casecmp(out + n - e, ext[i])) {
            out[n - e] = 0;
            return;
        }
    }
}

static uint32_t count_top(const ArcIndex *x)
{
    uint32_t n = 0;
    uint32_t i = 0;
    while (i < x->unique && n < 2) {
        const ArcItem *it = &x->items[x->order[i]];
        if (it->flags & ARC_F_UNSAFE) { i++; continue; }
        const char *name = arc_item_name(x, it), *slash = str_chr(name, '/');
        n++;
        if (!slash) { i++; continue; }
        char top[PATH_MAX];
        size_t l = (size_t)(slash - name) + 1;
        if (l >= sizeof top) break;
        __builtin_memcpy(top, name, l);
        top[l] = 0;
        do i++; while (i < x->unique && str_starts_with(arc_item_name(x, &x->items[x->order[i]]), top));
    }
    return n;
}

/* a whole archive with several top-level entries goes into <dst>/<archive name>/: -errno or 0 (u->root replaced) */
static int enter_stem_dir(ArcUnp *u, const char *archive)
{
    char stem[256];
    archive_stem(archive, stem);
    if (!stem[0]) (void)str_copy(stem, "archive", sizeof stem);
    intptr_t m = os_mkdirat(u->root, stem, 0755);
    if (m < 0 && m != -EEXIST) return (int)m;
    intptr_t fd = os_openat(u->root, stem, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC, 0);
    if (os_is_err(fd)) return (int)fd;
    (void)os_close(u->root);
    u->root = (int)fd;
    return 0;
}

/* the limits that need only the totals: -EFBIG */
static int check_limits(const ArcUnpackOpts *o, uint64_t bytes, uint64_t archive_size)
{
    if (o->max_bytes && bytes > o->max_bytes) return -EFBIG;
    if (o->bomb_ratio && bytes > o->bomb_floor && bytes / (archive_size ? archive_size : 1) > o->bomb_ratio) return -EFBIG;
    return 0;
}

static void publish_totals(uint64_t files, uint64_t bytes)
{
    TaskShared *shm = g_fs_progress;
    if (!shm) return;
    shm->total_files = files;
    shm->total_bytes = bytes;
    shm->phase = TASK_PHASE_RUN;
}

static intptr_t run_members(ArcUnp *u, const Sel *s)
{
    char rel[PATH_MAX];
    for (uint32_t i = 0; i < s->n; i++) {
        if (fs_progress_cancelled()) return -ECANCELED;
        const ArcItem *it = &u->idx.items[s->sel[i]];
        (void)str_copy(rel, arc_item_name(&u->idx, it) + u->selpfx, sizeof rel);
        size_t n = str_len(rel);
        if (n && rel[n - 1] == '/') rel[--n] = 0;
        if (!n) continue;                                                      /* the selected folder's own entry when it is the folder shown */
        fs_progress_begin(rel);
        int rc = arc_put_member(u, it, rel);
        fs_progress_item();
        if (rc < 0) return rc;
    }
    arc_put_dir_times(u);
    return 0;
}

intptr_t arc_unpack(const char *archive, const char *dst, const char *names, uint32_t count, const ArcUnpackOpts *o, ArcUnpackReport *r)
{
    mem_zero(r, sizeof *r);
    ArcUnp *u = mem_alloc(sizeof *u);
    if (!u) return -ENOMEM;
    mem_zero(u, sizeof *u);
    u->root = -1;
    u->o = o;
    u->r = r;
    Sel s = {nullptr, 0, nullptr, 0};
    volatile intptr_t rc = open_index(u, archive, r);
    FileStat st = {0};
    if (rc == 0) rc = select_items(u, &s, names, count);
    if (rc == 0 && os_statat(AT_FDCWD, archive, &st, 1) < 0) st.size = 0;
    if (rc == 0) rc = check_limits(o, s.bytes, (uint64_t)st.size);             /* nothing exists on disk yet */
    if (rc == 0) {
        intptr_t fd = open_root(dst);
        rc = os_is_err(fd) ? fd : 0;
        u->root = os_is_err(fd) ? -1 : (int)fd;
    }
    if (rc == 0 && o->smart_dir && !count && count_top(&u->idx) > 1) rc = enter_stem_dir(u, archive);
    uint64_t avail;
    if (rc == 0 && sys_fs_avail(u->root, &avail) == 0 && s.bytes > avail) rc = -ENOSPC_;
    if (rc == 0) {
        publish_totals(s.n, s.bytes);
        TryFrame f;
        if (os_try_begin(&f) == 0) {                                           /* a fault inside the mapped archive: stop, keep what is done */
            rc = run_members(u, &s);
            os_try_end(&f);
        } else {
            rc = -EIO;
        }
    }
    if (u->info.damaged) r->damaged = 1;
    mem_free(s.sel);
    mem_free(s.done);
    close_all(u);
    mem_free(u);
    return rc;
}

intptr_t arc_test(const char *archive, ArcUnpackReport *r)
{
    mem_zero(r, sizeof *r);
    ArcUnp *u = mem_alloc(sizeof *u);
    if (!u) return -ENOMEM;
    mem_zero(u, sizeof *u);
    u->root = -1;
    u->r = r;
    volatile intptr_t rc = open_index(u, archive, r);
    intptr_t null = rc == 0 ? os_openat(AT_FDCWD, "/dev/null", O_WRONLY | O_CLOEXEC, 0) : -ENOENT;
    if (rc == 0 && os_is_err(null)) rc = null;
    if (rc == 0) {
        uint64_t files = 0, bytes = 0;
        for (uint32_t i = 0; i < u->idx.unique; i++) {
            const ArcItem *it = &u->idx.items[u->idx.order[i]];
            if (!(it->flags & (ARC_F_DIR | ARC_F_SYMLINK | ARC_F_HARDLINK | ARC_F_SPECIAL | ARC_F_UNSAFE))) { files++; bytes += it->size; }
        }
        publish_totals(files, bytes);
        TryFrame f;
        if (os_try_begin(&f) == 0) {
            for (uint32_t i = 0; i < u->idx.unique && rc == 0; i++) {
                const ArcItem *it = &u->idx.items[u->idx.order[i]];
                if (it->flags & (ARC_F_DIR | ARC_F_SYMLINK | ARC_F_HARDLINK | ARC_F_SPECIAL | ARC_F_UNSAFE)) continue;
                if (fs_progress_cancelled()) { rc = -ECANCELED; break; }
                fs_progress_begin(arc_item_name(&u->idx, it));
                ArcXfer x = {arc_xfer_progress, nullptr, nullptr, 0, 0, 0};
                intptr_t e = arc_extract_to(&u->src, it, (int)null, &x);
                fs_progress_item();
                if (e == 0) {
                    r->files++;
                    r->bytes += it->size;
                } else if (x.sink_err == -ECANCELED) {
                    rc = -ECANCELED;
                } else {
                    r->skipped++;
                    if (!r->first_error) r->first_error = e;
                }
            }
            os_try_end(&f);
        } else {
            rc = -EIO;
        }
    }
    if (!os_is_err(null) && null >= 0) (void)os_close((int)null);
    if (u->info.damaged) r->damaged = 1;
    close_all(u);
    mem_free(u);
    return rc;
}
