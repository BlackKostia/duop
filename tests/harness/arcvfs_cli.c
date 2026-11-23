/* Freestanding harness for the archive virtual folder (src/fs/archive_vfs.c + arc_source.c + arc_extract.c) on the C DirModel stack.
 * Script on stdin (names as shown in the pane):
 *   N <dir>   a pane model of dir     E <name>  arc_enter_model on the visible entry      U   arc_nav_up_model
 *   X <name>  arc_extract_entry: "X r=<rc> len=<bytes> crc=<crc32>" (the temp file is removed)      L   the listing
 * The listing is " path=<p> vm=<mode> n=<count> sub=<folder> name:flags:size ..." (flags: 1 dir, 32 parent; names escaped). */
#include "script_cli.h"
#include "duop_arcvfs.h"
#include "duop_hash.h"
#include "duop_pal.h"

int git_refresh(DirModel *m) { (void)m; return 0; }
void git_free_ctx(DirModel *m) { m->git_ctx = nullptr; }

static DirModel *g_m;
static char g_tmp[PATH_MAX];

static int sc_split(char *line, char **f, int max)
{
    int n = 0;
    while (*line && n < max) {
        while (*line == ' ') *line++ = 0;
        if (!*line) break;
        f[n++] = line;
        while (*line && *line != ' ') line++;
    }
    return n;
}

static FileEntry *find_visible(const char *name)
{
    intptr_t i = DirModel_find_by_name(g_m, name);
    return i < 0 ? nullptr : DirModel_get_entry(g_m, i);
}

static void put_listing(void)
{
    sc_put(" path="); sc_esc(g_m->path); sc_put(" vm="); sc_num(g_m->virtual_mode); sc_put(" n="); sc_num((int64_t)g_m->count);
    sc_put(" sub="); sc_esc(g_m->arc_ctx ? arc_vfs_sub(g_m) : "-");
    for (uint64_t i = 0; i < g_m->count; i++) {
        const FileEntry *e = &g_m->entries[g_m->order[i]];
        sc_put(" "); sc_esc(g_m->name_pool + e->name_off); sc_put(":"); sc_num(e->flags & (FE_IS_DIR | FE_IS_PARENT)); sc_put(":"); sc_num(e->size);
    }
}

static void hash_extracted(void)
{
    static uint8_t buf[1 << 20];
    intptr_t fd = os_openat(AT_FDCWD, g_tmp, O_RDONLY | O_CLOEXEC, 0);
    uint64_t total = 0;
    uint32_t crc = 0xFFFFFFFFu;
    for (; !os_is_err(fd);) {
        intptr_t r = os_read((int)fd, buf, sizeof buf);
        if (r <= 0) break;
        crc = crc32_update(crc, buf, (size_t)r);
        total += (uint64_t)r;
    }
    if (!os_is_err(fd)) (void)os_close((int)fd);
    sc_put(" len="); sc_num((int64_t)total); sc_put(" crc="); sc_num((int64_t)(~crc));
    (void)os_unlinkat(AT_FDCWD, g_tmp, 0);
}

int duop_main(int argc, char **argv, char **envp, const uintptr_t *auxv)
{
    (void)argc; (void)argv; (void)envp; (void)auxv;
    os_fault_guard_install();
    char *script = sc_read_all();
    for (char *line; (line = sc_next_line(&script));) {
        char *f[4];
        int n = sc_split(line, f, 4);
        if (!n) continue;
        switch (f[0][0]) {
        case 'N': if (g_m) Object_free(&g_m->base); g_m = DirModel_new(f[1]); if (g_m) { while (g_m->is_loading) (void)DirModel_step_load(g_m, 0); }
                  sc_put("N"); if (g_m) put_listing(); break;
        case 'E': { if (!g_m) continue; FileEntry *e = n > 1 ? find_visible(f[1]) : nullptr; if (e) DirModel_ensure_entry_stat(g_m, e);
                    int r = arc_enter_model(g_m, e); sc_put("E r="); sc_num(r); put_listing(); break; }
        case 'U': { if (!g_m) continue; int r = arc_nav_up_model(g_m); while (g_m->is_loading) (void)DirModel_step_load(g_m, 0);
                    sc_put("U r="); sc_num(r); put_listing(); break; }
        case 'X': {
            if (!g_m || n < 2) continue;
            intptr_t r = arc_extract_entry(g_m, find_visible(f[1]), g_tmp);
            sc_put("X r="); sc_num(r);
            if (r == 0) hash_extracted();
            break;
        }
        case 'L': if (!g_m) continue; sc_put("L"); put_listing(); break;
        default: continue;
        }
        sc_put("\n");
        sc_flush();
    }
    if (g_m) Object_free(&g_m->base);
    sc_flush();
    return 0;
}
