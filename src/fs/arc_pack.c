/* src/fs/arc_pack.c - creating an archive from files and folders (freestanding C23): the walk, the entries, the progress.
 *
 * The walk is iterative (a stack of open folders, at most 64 deep) and sorted by name, so the order of an archive does not depend on the order
 * the file system lists names in. Nothing is followed: every entry is looked at with lstat, a file is opened with O_NOFOLLOW and checked
 * against that lstat (same device and inode), links are stored as links. A first pass only adds up the totals for the progress bar. The output
 * side - the temporary sibling, gzip, the rename - is arc_pack_out.c; the formats are tar_write.c and zip_write.c. */
#include "duop_arcpack_int.h"
#include "duop_fs.h"
#include "duop_oop.h"
#include "duop_pal.h"
#include "duop_string.h"
#include "duop_sys.h"
#include "duop_task.h"

enum { MAX_DEPTH = 64, READ_BUF = 256 << 10, ENXIO_ = 6, ELOOP_ = 40 };

typedef struct DirList {
    char *arena;
    size_t len, cap;
    char **v;
    uint32_t n;
} DirList;

typedef struct Frame {
    int fd;
    DirList list;
    uint32_t i;
    size_t parent_len;                 /* the length of the archive path to go back to when the folder is done */
} Frame;

typedef struct Pack {
    const ArcPackOpts *o;
    ArcPackReport *r;
    PackOut out;
    TarWriter *tar;
    ZipWriter *zip;
    uint8_t *buf;
    Frame frames[MAX_DEPTH];
    int depth;
    bool dry;                          /* the first pass: totals only */
    uint64_t tot_files, tot_bytes;
    size_t plen;                       /* length of the archive path of the current folder, "" or "a/b/" */
    char path[PATH_MAX], ent[PATH_MAX], link[PATH_MAX];
} Pack;

static int skip(Pack *p, intptr_t err)
{
    p->r->skipped++;
    if (!p->r->first_error) p->r->first_error = err;
    return 0;
}

/* ---- one folder, listed and sorted ---- */
static void sort_names(char **v, uint32_t n)
{
    static const uint32_t gaps[] = {701, 301, 132, 57, 23, 10, 4, 1};
    for (size_t g = 0; g < sizeof gaps / sizeof gaps[0]; g++)
        for (uint32_t i = gaps[g]; i < n; i++) {
            char *t = v[i];
            uint32_t j = i;
            for (; j >= gaps[g] && str_cmp(v[j - gaps[g]], t) > 0; j -= gaps[g]) v[j] = v[j - gaps[g]];
            v[j] = t;
        }
}

static int list_dir(int fd, DirList *l)
{
    uint8_t *buf = mem_alloc(32768);
    if (!buf) return -ENOMEM;
    int rc = 0;
    for (;;) {
        intptr_t r = os_dir_read(fd, buf, 32768);
        if (r < 0) rc = (int)r;
        if (r <= 0) break;
        size_t pos = 0;
        for (const char *nm; (nm = os_dir_next(buf, (size_t)r, &pos));) {
            size_t k = str_len(nm) + 1;
            if ((nm[0] == '.' && !nm[1]) || (nm[0] == '.' && nm[1] == '.' && !nm[2])) continue;
            if (l->len + k > l->cap) {
                size_t cap = l->cap ? l->cap * 2 : 4096;
                while (cap < l->len + k) cap *= 2;
                char *a = mem_realloc(l->arena, cap);
                if (!a) {
                    rc = -ENOMEM;
                    break;
                }
                l->arena = a;
                l->cap = cap;
            }
            __builtin_memcpy(l->arena + l->len, nm, k);
            l->len += k;
            l->n++;
        }
        if (rc) break;
    }
    mem_free(buf);
    if (rc || !l->n) return rc;
    l->v = mem_alloc((size_t)l->n * sizeof *l->v);                 /* the pointers, once the arena stopped moving */
    if (!l->v) return -ENOMEM;
    size_t off = 0;
    for (uint32_t i = 0; i < l->n; i++) {
        l->v[i] = l->arena + off;
        off += str_len(l->arena + off) + 1;
    }
    sort_names(l->v, l->n);
    return 0;
}

static void free_list(DirList *l)
{
    mem_free(l->arena);
    mem_free(l->v);
    mem_zero(l, sizeof *l);
}

/* ---- entries ---- */
static void meta(const Pack *p, const FileStat *st, PackEntry *e, char type)
{
    bool fixed = p->o->flags & ARC_PACK_FIXED;
    e->type = type;
    e->mtime = fixed ? p->o->mtime : st->mtime;
    e->uid = fixed ? 0 : st->uid;
    e->gid = fixed ? 0 : st->gid;
    e->mode = type == '2' ? 0777 : fixed ? (type == '5' || (st->mode & 0111) ? 0755 : 0644) : st->mode & 07777;
}

static int begin(Pack *p, const PackEntry *e) { return p->tar ? tar_begin(p->tar, e) : zip_begin(p->zip, e); }
static int data(Pack *p, const uint8_t *b, size_t n) { return p->tar ? tar_data(p->tar, b, n) : zip_data(p->zip, b, n); }
static int end(Pack *p) { return p->tar ? tar_end(p->tar) : zip_end(p->zip); }

static int put_dir(Pack *p, const FileStat *st)
{
    if (p->dry) return 0;
    PackEntry e = {p->ent, nullptr, 0, 0, 0, 0, 0, '5'};
    meta(p, st, &e, '5');
    int r = begin(p, &e);
    if (!r) r = end(p);
    p->r->dirs++;
    return r;
}

static int put_link(Pack *p, int dfd, const char *name, const FileStat *st)
{
    intptr_t n = os_readlinkat(dfd, name, p->link, sizeof p->link - 1);
    if (n < 0) return skip(p, n);
    p->link[n] = 0;
    if (p->dry) return 0;
    PackEntry e = {p->ent, p->link, (uint64_t)n, 0, 0, 0, 0, '2'};
    meta(p, st, &e, '2');
    int r = begin(p, &e);
    if (!r && p->zip) r = data(p, (const uint8_t *)p->link, (size_t)n);
    if (!r) r = end(p);
    p->r->links++;
    return r;
}

static int put_file(Pack *p, int dfd, const char *name, const FileStat *st)
{
    if (p->dry) {
        p->tot_files++;
        p->tot_bytes += (uint64_t)st->size;
        return 0;
    }
    intptr_t fd = os_openat(dfd, name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC, 0);
    if (os_is_err(fd)) return skip(p, fd);
    FileStat now;
    if (os_fstat((int)fd, &now) < 0 || now.dev != st->dev || now.ino != st->ino) {   /* replaced between lstat and open */
        (void)os_close((int)fd);
        p->r->changed++;
        return skip(p, -EAGAIN);
    }
    PackEntry e = {p->ent, nullptr, (uint64_t)now.size, 0, 0, 0, 0, '0'};
    meta(p, &now, &e, '0');
    fs_progress_begin(p->ent);
    int r = begin(p, &e);
    uint64_t left = e.size;
    while (!r && left) {
        if (fs_progress_cancelled()) {
            r = -ECANCELED;
            break;
        }
        intptr_t n = sys_read_retry((int)fd, p->buf, left < READ_BUF ? (size_t)left : READ_BUF);
        if (n <= 0) {                                              /* it shrank, or the read failed: the entry ends short */
            p->r->changed++;
            if (n < 0) (void)skip(p, n);
            break;
        }
        r = data(p, p->buf, (size_t)n);
        left -= (uint64_t)n;
        p->r->bytes_in += (uint64_t)n;
        fs_progress_bytes((uint64_t)n);
    }
    if (!r) r = end(p);
    if (!r && !left && os_fstat((int)fd, &now) == 0 && (uint64_t)now.size != e.size) p->r->changed++;     /* it grew meanwhile */
    (void)os_close((int)fd);
    p->r->files++;
    fs_progress_item();
    return r;
}

static int push_dir(Pack *p, int dfd, const char *name, size_t parent_len)
{
    if (p->depth >= MAX_DEPTH) return skip(p, -ELOOP_);
    intptr_t fd = os_openat(dfd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC, 0);
    if (os_is_err(fd)) return skip(p, fd);
    Frame *f = &p->frames[p->depth];
    mem_zero(f, sizeof *f);
    f->fd = (int)fd;
    f->parent_len = parent_len;
    int r = list_dir((int)fd, &f->list);
    if (r) {
        free_list(&f->list);
        (void)os_close((int)fd);
        return r == -ENOMEM ? r : skip(p, r);
    }
    p->depth++;
    return 0;
}

/* one name in the folder open on dfd: its entry, and for a folder the frame that will walk it */
static int visit(Pack *p, int dfd, const char *name)
{
    if (fs_progress_cancelled()) return -ECANCELED;
    FileStat st;
    int r = os_statat(dfd, name, &st, 0);
    if (r < 0) return skip(p, r);
    if (st.dev == p->out.dev && st.ino == p->out.ino) return 0;      /* the archive being written */
    size_t base = p->plen, nl = str_len(name);
    if (base + nl + 2 >= PATH_MAX) return skip(p, -ENAMETOOLONG);
    __builtin_memcpy(p->ent, p->path, base);
    __builtin_memcpy(p->ent + base, name, nl + 1);
    uint32_t type = st.mode & S_IFMT;
    if (type == S_IFDIR) {
        p->ent[base + nl] = '/';
        p->ent[base + nl + 1] = 0;
        r = put_dir(p, &st);
        if (r) return r;
        int before = p->depth;
        r = push_dir(p, dfd, name, base);
        if (r) return r;
        if (p->depth > before) {                                     /* walking into it: its children are named below it */
            __builtin_memcpy(p->path, p->ent, base + nl + 2);
            p->plen = base + nl + 1;
        }
        return 0;
    }
    if (type == S_IFREG) return put_file(p, dfd, name, &st);
    if (type == S_IFLNK) return put_link(p, dfd, name, &st);
    return skip(p, -ENXIO_);                                          /* device, fifo, socket */
}

/* the whole walk: the named entries of the base folder, then every folder pushed on the way */
static int walk(Pack *p, int base_fd, const char *names, uint32_t count)
{
    int rc = 0;
    p->plen = 0;
    p->path[0] = 0;
    for (uint32_t k = 0; !rc && k < count; k++, names += str_len(names) + 1) {
        rc = visit(p, base_fd, names);
        while (!rc && p->depth > 0) {
            Frame *f = &p->frames[p->depth - 1];
            if (f->i == f->list.n) {
                p->plen = f->parent_len;
                p->path[p->plen] = 0;
                free_list(&f->list);
                (void)os_close(f->fd);
                p->depth--;
                continue;
            }
            rc = visit(p, f->fd, f->list.v[f->i++]);
        }
    }
    while (p->depth > 0) {                                           /* an error or a cancel: close what is open */
        Frame *f = &p->frames[--p->depth];
        free_list(&f->list);
        (void)os_close(f->fd);
    }
    return rc;
}

int arc_pack_format(const char *name)
{
    static const struct { const char *sfx; int fmt; } t[] = {{".zip", ARC_PACK_ZIP}, {".tar.gz", ARC_PACK_TGZ}, {".tgz", ARC_PACK_TGZ}, {".tar", ARC_PACK_TAR}};
    size_t n = str_len(name);
    for (size_t i = 0; i < sizeof t / sizeof t[0]; i++) {
        size_t k = str_len(t[i].sfx);
        if (n > k && !str_casecmp(name + n - k, t[i].sfx)) return t[i].fmt;
    }
    return -1;
}

static void publish_totals(const Pack *p)
{
    TaskShared *shm = g_fs_progress;
    if (!shm) return;
    shm->total_files = p->tot_files;
    shm->total_bytes = p->tot_bytes;
    shm->phase = TASK_PHASE_RUN;
}

static int run(Pack *p, int base_fd, const char *names, uint32_t count, uint32_t format)
{
    p->dry = true;
    int rc = walk(p, base_fd, names, count);
    if (rc) return rc;
    publish_totals(p);
    p->dry = false;
    p->r->skipped = 0;
    p->r->first_error = 0;
    if (format == ARC_PACK_ZIP) {
        p->zip = zip_new(&p->out.top, (int)p->o->level);
        if (!p->zip) return -ENOMEM;
    } else {
        p->tar = mem_alloc(sizeof *p->tar);
        if (!p->tar) return -ENOMEM;
        p->tar->out = &p->out.top;
    }
    rc = walk(p, base_fd, names, count);
    if (!rc) rc = p->tar ? tar_finish(p->tar) : zip_finish(p->zip);
    return rc;
}

intptr_t arc_pack(const char *dst, const char *names, uint32_t count, const ArcPackOpts *o, ArcPackReport *r)
{
    mem_zero(r, sizeof *r);
    if (o->format > ARC_PACK_ZIP || !count) return -EINVAL;
    intptr_t base = os_openat(AT_FDCWD, names, O_RDONLY | O_DIRECTORY | O_CLOEXEC, 0);
    if (os_is_err(base)) return base;
    Pack *p = mem_alloc(sizeof *p);
    if (!p) {
        (void)os_close((int)base);
        return -ENOMEM;
    }
    p->buf = mem_alloc(READ_BUF);
    if (!p->buf) {
        mem_free(p);
        (void)os_close((int)base);
        return -ENOMEM;
    }
    ArcPackOpts opts = *o;
    if (opts.level > 9) opts.level = 6;
    p->o = &opts;
    p->r = r;
    int rc = pack_out_open(&p->out, dst, opts.format, opts.level);
    if (!rc) rc = run(p, (int)base, names + str_len(names) + 1, count, opts.format);
    if (p->out.fd >= 0 || p->out.def) {
        int cr = pack_out_close(&p->out, rc == 0);
        if (!rc) rc = cr;
    }
    r->bytes_out = p->out.bytes_out;
    zip_free(p->zip);
    mem_free(p->tar);
    mem_free(p->buf);
    mem_free(p);
    (void)os_close((int)base);
    return rc;
}
