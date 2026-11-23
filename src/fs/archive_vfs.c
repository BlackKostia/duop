/* src/fs/archive_vfs.c - an archive shown as a virtual folder of a pane, and single members extracted to temp files (freestanding C23).
 *
 * Entering an archive maps it (arc_source.c), indexes it once (arc_tar.c / arc_zip.c into an ArcIndex, sorted) and lists one level of
 * v->sub at a time: the children of a folder are found by binary search, so a folder of a million members opens as fast as a small
 * one. ".." walks up and finally back to the real directory. Parsing and extraction run under the fault guard: a corrupt or
 * shrinking archive can never take the UI down. There is no limit on the size of the archive, of a member or on their number. */
#include "duop_arcvfs.h"
#include "duop_pal.h"
#include "duop_string.h"
#include "duop_sys.h"

static ArcVfs *vfs_of(const DirModel *m) { return (ArcVfs *)m->arc_ctx; }

const char *arc_vfs_sub(const DirModel *m) { return vfs_of(m)->sub; }

const char *arc_vfs_orig(const DirModel *m) { return vfs_of(m)->orig; }

typedef struct {
    DirModel *m;
    const ArcVfs *v;
    char *name, *key;               /* PATH_MAX scratch each */
} Pop;

/* one child of the folder: a file or folder row (a folder whose name is also a file is not listed twice: the file wins) */
static int populate_cb(void *ctx, const char *comp, size_t clen, bool is_dir, const ArcItem *it)
{
    Pop *p = ctx;
    if (clen >= PATH_MAX) return 0;
    __builtin_memcpy(p->name, comp, clen);
    p->name[clen] = 0;
    if (is_dir) {
        (void)str_copy(p->key, p->v->sub, PATH_MAX);
        (void)str_cat(p->key, p->name, PATH_MAX);
        if (arc_idx_find(&p->v->idx, p->key) && is_dir) return 0;
    }
    FileStat st = {0};
    if (is_dir) {
        st.mode = S_IFDIR | 0755;
        if (it) st.mtime = it->mtime;
    } else {
        bool link = it->flags & ARC_F_SYMLINK;
        st.mode = (link ? S_IFLNK : S_IFREG) | (it->mode ? (it->mode & 0777) : 0644);
        st.size = (int64_t)it->size;
        st.mtime = it->mtime;
    }
    return ds_append_entry(p->m, p->name, &st) < 0 ? -ENOMEM : 0;
}

/* the listing of v->sub; filter and sort */
void arc_populate(DirModel *m)
{
    ArcVfs *v = vfs_of(m);
    DirModel_cancel_load(m);
    m->raw_count = m->count = m->name_used = m->total_bytes = 0;
    m->dir_count = m->file_count = 0;
    m->virtual_mode = 2;
    m->stats_complete = 1;
    (void)str_copy(m->path, v->file, PATH_MAX);
    (void)ds_append_parent(m);
    char *scratch = mem_alloc(2 * PATH_MAX);
    if (scratch) {
        Pop p = {m, v, scratch, scratch + PATH_MAX};
        (void)arc_idx_list(&v->idx, v->sub, populate_cb, &p);
        mem_free(scratch);
    }
    DirModel_apply_filter(m);
    DirModel_sort(m);
}

void arc_free_ctx(DirModel *m)
{
    ArcVfs *v = vfs_of(m);
    if (!v) return;
    m->arc_ctx = nullptr;
    arc_idx_free(&v->idx);
    arc_source_close(&v->src);
    mem_free(v);
}

/* ".." inside an archive: one folder up, or out of the archive (the cursor lands on the archive file) */
int arc_nav_up_model(DirModel *m)
{
    if (m->virtual_mode != 2) return -1;
    ArcVfs *v = vfs_of(m);
    size_t n = str_len(v->sub);
    if (n) {
        size_t i = n - 1;
        while (i && v->sub[i - 1] != '/') i--;
        v->sub[i] = 0;                                                  /* "a/b/" -> "a/", "a/" -> "" */
        arc_populate(m);
        return 0;
    }
    char *mem = mem_alloc(2 * PATH_MAX);                               /* the directory and the archive name outlive the context */
    if (!mem) return 0;
    (void)str_copy(mem, v->orig, PATH_MAX);
    (void)str_copy(mem + PATH_MAX, path_basename(v->file), PATH_MAX);
    m->virtual_mode = 0;
    arc_free_ctx(m);
    (void)DirModel_chdir(m, mem);
    intptr_t cur = DirModel_find_by_name(m, mem + PATH_MAX);
    mem_free(mem);
    return (int32_t)cur >= 0 ? (int)cur : 0;
}

/* maps and indexes the archive into v (the fault guard around the parsers): 0 or -errno */
static int arc_index_file(ArcVfs *v, const char *path, int type)
{
    int r = arc_source_open(&v->src, path, type);
    if (r) return r;
    arc_idx_init(&v->idx);
    TryFrame f;
    volatile int rc = -EIO;
    if (os_try_begin(&f) == 0) {                                       /* a fault while indexing: the archive is dropped */
        const ArcSource *s = &v->src;
        rc = type == ARC_ZIP ? arc_zip_parse(&v->idx, s->data, s->len, &v->info) : arc_tar_parse(&v->idx, s->data, s->len, &v->info);
        os_try_end(&f);
    }
    if (rc == 0) rc = arc_idx_finish(&v->idx);
    return rc;
}

static int arc_open(DirModel *m, const char *name)
{
    int type = arc_is_archive(name);
    char *path = mem_alloc(PATH_MAX);
    if (!type || !path || path_join(path, m->path, name, PATH_MAX) < 0) {   /* path-checked */
        mem_free(path);
        return 0;
    }
    arc_free_ctx(m);
    ArcVfs *v = mem_alloc(sizeof *v);
    if (!v) { mem_free(path); return 0; }
    mem_zero(v, sizeof *v);
    if (arc_index_file(v, path, type) != 0) {
        arc_idx_free(&v->idx);
        arc_source_close(&v->src);
        mem_free(v);
        mem_free(path);
        return 0;
    }
    (void)str_copy(v->orig, m->path, PATH_MAX);
    (void)str_copy(v->file, path, PATH_MAX);
    mem_free(path);
    m->arc_ctx = v;
    arc_populate(m);
    return v->info.damaged ? 3 : 1;
}

int arc_enter_model(DirModel *m, const FileEntry *ent)
{
    if (!ent) return 0;
    const char *name = DirModel_entry_name(m, ent);
    if (m->virtual_mode != 2) {
        if (ent->flags & FE_IS_DIR) return 0;
        return arc_open(m, name);
    }
    if (ent->flags & FE_IS_PARENT) return 2;
    if (!(ent->flags & FE_IS_DIR)) return 0;
    ArcVfs *v = vfs_of(m);                                              /* one folder deeper */
    size_t n = str_len(v->sub);
    if (n + str_len(name) + 2 > PATH_MAX) return 0;
    (void)str_cat(v->sub, name, PATH_MAX);
    n = str_len(v->sub);
    v->sub[n] = '/';
    v->sub[n + 1] = 0;
    arc_populate(m);
    return 1;
}

/* the member shown as `ent` into a new temp file (its path in out_tmp_path): 0, or -errno (the temp file is removed) */
intptr_t arc_extract_entry(DirModel *m, const FileEntry *ent, char *out_tmp_path)
{
    if (m->virtual_mode != 2 || !ent || (ent->flags & FE_IS_DIR)) return -EINVAL;
    const ArcVfs *v = vfs_of(m);
    char *key = mem_alloc(PATH_MAX);
    if (!key) return -ENOMEM;
    (void)str_copy(key, v->sub, PATH_MAX);
    (void)str_cat(key, DirModel_entry_name(m, ent), PATH_MAX);
    const ArcItem *it = arc_idx_find(&v->idx, key);
    mem_free(key);
    if (!it) return -ENOENT;
    volatile intptr_t rc = -EIO;
    intptr_t fd = tmp_open_unique("arc_view", ".tmp", out_tmp_path, PATH_MAX);
    if (os_is_err(fd)) return fd;
    TryFrame f;
    if (os_try_begin(&f) == 0) {                                        /* the archive may shrink under its mapping */
        rc = arc_extract_item(&v->src, it, (int)fd);
        os_try_end(&f);
    }
    (void)os_close((int)fd);
    if (rc != 0) (void)os_unlinkat(AT_FDCWD, out_tmp_path, 0);
    return rc;
}
