/* Freestanding differential harness for src/ui/file_pane.c versus frozen x86_64 asm
 * (tests/harness/file_pane_reference.s, ref_ prefix). argv[1] = ref | c. */
#include "duop_classes.h"
#include "duop_du.h"
#include "duop_fs.h"
#include "duop_pal.h"
#include "duop_string.h"
#include "duop_tui.h"
#include "duop_ui.h"
#include "script_cli.h"

void arc_populate(DirModel *m) { (void)m; sc_put("call arc_populate\n"); }

static App g_app;
static int g_fail_obj;
static int g_fail_dm;
static int g_exit_virt;
static intptr_t g_chdir_err;
static int g_reload_shift;
static FileEntry g_ents[4];
static uint32_t g_ord[4] = {0, 1, 2, 3};
static char g_pool[] = "..\0alpha\0child\0zebra\0";
static NavTab g_tab_slot;

/* Virtual methods referenced by FilePane_vtbl (not called here) */
void FilePane_layout(Widget *s, int x, int y, int w, int h) { (void)s; (void)x; (void)y; (void)w; (void)h; }
void FilePane_render(Widget *s, void *scr, void *th) { (void)s; (void)scr; (void)th; }
int FilePane_on_key(Widget *s, void *ev) { (void)s; (void)ev; return 0; }
int FilePane_on_mouse(Widget *s, void *ev) { (void)s; (void)ev; return 0; }

static void stub_dm_dtor(Object *o) { (void)o; sc_put("call DirModel_dtor\n"); }
static const ObjectVtbl StubDM_vtbl = {&Object_vtbl, "DirModel", sizeof(DirModel), stub_dm_dtor};

void *fake_Object_alloc(const ObjectVtbl *vtbl, size_t size);
void *fake_Object_alloc(const ObjectVtbl *vtbl, size_t size)
{
    if (g_fail_obj > 0 && --g_fail_obj == 0) return nullptr;
    return Object_alloc(vtbl, size);
}

DirModel *fake_DirModel_new(const char *path);
DirModel *fake_DirModel_new(const char *path)
{
    sc_put("call DirModel_new |"); sc_esc(path ? path : ""); sc_put("|\n");
    if (g_fail_dm) return nullptr;
    DirModel *m = Object_alloc(&StubDM_vtbl, sizeof(DirModel));
    if (!m) return nullptr;
    m->entries = g_ents;
    m->order = g_ord;
    m->name_pool = g_pool;
    m->count = 4;
    (void)str_copy(m->path, path ? path : "/", PATH_MAX);
    return m;
}

NavTab *fake_nav_tabs_alloc(const char *init_path);
NavTab *fake_nav_tabs_alloc(const char *init_path)
{
    sc_put("call nav_tabs_alloc |"); sc_esc(init_path ? init_path : ""); sc_put("|\n");
    mem_zero(&g_tab_slot, sizeof g_tab_slot);
    (void)str_copy(g_tab_slot.path, init_path ? init_path : "", PATH_MAX);
    return &g_tab_slot;
}

void fake_nav_tabs_free(NavTab *tabs, int cnt);
void fake_nav_tabs_free(NavTab *tabs, int cnt)
{
    sc_put("call nav_tabs_free ok="); sc_num(tabs == &g_tab_slot); sc_put(" cnt="); sc_num(cnt); sc_put("\n");
}

FileEntry *fake_DirModel_get_entry(DirModel *m, intptr_t idx);
FileEntry *fake_DirModel_get_entry(DirModel *m, intptr_t idx)
{
    if (idx < 0 || (uint64_t)idx >= m->count) return nullptr;
    return &m->entries[m->order[idx]];
}

void fake_DirModel_ensure_entry_stat(DirModel *m, FileEntry *e);
void fake_DirModel_ensure_entry_stat(DirModel *m, FileEntry *e)
{
    (void)m;
    sc_put("call ensure_entry_stat off="); sc_num(e->name_off); sc_put("\n");
}

const char *fake_DirModel_entry_name(const DirModel *m, const FileEntry *e);
const char *fake_DirModel_entry_name(const DirModel *m, const FileEntry *e)
{
    return m->name_pool + e->name_off;
}

int fake_DirModel_reload(DirModel *m);
int fake_DirModel_reload(DirModel *m)
{
    sc_put("call DirModel_reload shift="); sc_num(g_reload_shift); sc_put("\n");
    if (g_reload_shift) {
        g_ord[0] = 0; g_ord[1] = 3; g_ord[2] = 1; g_ord[3] = 2; /* alpha moved from 1 -> 2 */
    } else {
        g_ord[0] = 0; g_ord[1] = 1; g_ord[2] = 2; g_ord[3] = 3;
    }
    (void)m;
    return 0;
}

intptr_t fake_DirModel_find_by_name(DirModel *m, const char *name);
intptr_t fake_DirModel_find_by_name(DirModel *m, const char *name)
{
    for (uint64_t i = 0; i < m->count; i++) {
        const char *en = m->name_pool + m->entries[m->order[i]].name_off;
        if (str_cmp(en, name) == 0) return (intptr_t)i;
    }
    return -1;
}

int fake_fp_exit_virtual(FilePane *fp);
int fake_fp_exit_virtual(FilePane *fp)
{
    (void)fp;
    sc_put("call fp_exit_virtual -> "); sc_num(g_exit_virt); sc_put("\n");
    return g_exit_virt;
}

intptr_t fake_DirModel_chdir(DirModel *m, const char *target);
intptr_t fake_DirModel_chdir(DirModel *m, const char *target)
{
    sc_put("call DirModel_chdir |"); sc_esc(target); sc_put("| err="); sc_num(g_chdir_err); sc_put("\n");
    if (g_chdir_err < 0) return g_chdir_err;
    if (str_cmp(target, "..") == 0) {
        char par[PATH_MAX];
        path_dirname(m->path, par, sizeof par);
        (void)str_copy(m->path, par, PATH_MAX);
    }
    return 0;
}

int fake_dir_du_populate(DirModel *m);
int fake_dir_du_populate(DirModel *m)
{
    (void)m;
    sc_put("call dir_du_populate\n");
    return 1;
}

NavTab *fake_fp_get_active_tab(FilePane *fp);
NavTab *fake_fp_get_active_tab(FilePane *fp)
{
    return fp->tabs ? (NavTab *)(uintptr_t)fp->tabs : nullptr;
}

void fake_nav_hist_push(NavTab *t, const char *path);
void fake_nav_hist_push(NavTab *t, const char *path)
{
    sc_put("call nav_hist_push ok="); sc_num(t == &g_tab_slot); sc_put(" |"); sc_esc(path); sc_put("|\n");
}

void fake_nav_frecency_record(const char *path);
void fake_nav_frecency_record(const char *path)
{
    sc_put("call nav_frecency_record |"); sc_esc(path); sc_put("|\n");
}

#define Object_alloc fake_Object_alloc
#define DirModel_new fake_DirModel_new
#define nav_tabs_alloc fake_nav_tabs_alloc
#define nav_tabs_free fake_nav_tabs_free
#define DirModel_get_entry fake_DirModel_get_entry
#define DirModel_ensure_entry_stat fake_DirModel_ensure_entry_stat
#define DirModel_entry_name fake_DirModel_entry_name
#define DirModel_reload fake_DirModel_reload
#define DirModel_find_by_name fake_DirModel_find_by_name
#define fp_exit_virtual fake_fp_exit_virtual
#define DirModel_chdir fake_DirModel_chdir
#define dir_du_populate fake_dir_du_populate
#define fp_get_active_tab fake_fp_get_active_tab
#define nav_hist_push fake_nav_hist_push
#define nav_frecency_record fake_nav_frecency_record
#include FP_SRC
#undef nav_frecency_record
#undef nav_hist_push
#undef fp_get_active_tab
#undef dir_du_populate
#undef DirModel_chdir
#undef fp_exit_virtual
#undef DirModel_find_by_name
#undef DirModel_reload
#undef DirModel_entry_name
#undef DirModel_ensure_entry_stat
#undef DirModel_get_entry
#undef nav_tabs_free
#undef nav_tabs_alloc
#undef DirModel_new
#undef Object_alloc

#ifdef __x86_64__
extern FilePane *ref_FilePane_new(App *app, const char *path, int id);
extern int ref_get_list_h(FilePane *fp);
extern void ref_FilePane_ensure_visible(FilePane *fp);
extern FileEntry *ref_FilePane_get_selected_entry(FilePane *fp);
extern void ref_FilePane_reload_keep_cursor(FilePane *fp, const char *name);
extern void ref_FilePane_enter_parent(FilePane *fp);
#define CALL(f, ...) (g_ref ? ref_##f(__VA_ARGS__) : f(__VA_ARGS__))
#else
#define CALL(f, ...) f(__VA_ARGS__)
#endif

static bool g_ref;

static int64_t parse_i(const char **p)
{
    while (**p == ' ') (*p)++;
    int neg = 0;
    if (**p == '-') { neg = 1; (*p)++; }
    int64_t v = 0;
    while (**p >= '0' && **p <= '9') { v = v * 10 + (**p - '0'); (*p)++; }
    return neg ? -v : v;
}

int duop_main(int argc, char **argv, char **envp, const uintptr_t *auxv)
{
    (void)envp; (void)auxv;
    if (argc < 2) return 2;
    g_ref = argv[1][0] == 'r';

    g_ents[0].name_off = 0;  /* ".." */
    g_ents[1].name_off = 3;  /* "alpha" */
    g_ents[2].name_off = 9;  /* "child" */
    g_ents[3].name_off = 15; /* "zebra" */

    /* OOM on FilePane object */
    g_fail_obj = 1;
    FilePane *oom_fp = CALL(FilePane_new, &g_app, "/a/child", 1);
    sc_put("OOM_FP="); sc_num(oom_fp == nullptr); sc_put("\n");

    /* OOM on DirModel_new -> frees fp and returns NULL */
    g_fail_dm = 1;
    FilePane *oom_dm = CALL(FilePane_new, &g_app, "/a/child", 1);
    sc_put("OOM_DM="); sc_num(oom_dm == nullptr); sc_put("\n");
    g_fail_dm = 0;

    FilePane *fp = CALL(FilePane_new, &g_app, "/a/child", 7);
    if (!fp) return 3;
    sc_put("NEW id="); sc_num(fp->base.id);
    sc_put(" lci="); sc_num((int32_t)fp->last_click_idx);
    sc_put(" hr="); sc_num((int32_t)fp->hovered_row);
    sc_put(" hh="); sc_num((int32_t)fp->hovered_col_hdr);
    sc_put(" tc="); sc_num(fp->tab_count);
    sc_put(" cls="); sc_put(((const ObjectVtbl *)fp->base.base.vptr)->class_name);
    sc_put("\n");

    DirModel *m = (DirModel *)(uintptr_t)fp->model;
    char *script = sc_read_all();
    for (char *line; (line = sc_next_line(&script));) {
        const char *a = line + (line[0] ? 1 : 0);
        switch (line[0]) {
        case 'H': {
            fp->base.h = (int32_t)parse_i(&a);
            sc_put("H "); sc_num(fp->base.h); sc_put(" -> "); sc_num(CALL(get_list_h, fp)); sc_put("\n");
            break;
        }
        case 'V': {
            m->count = (uint64_t)parse_i(&a);
            fp->base.h = (int32_t)parse_i(&a);
            fp->cursor = (uint32_t)parse_i(&a);
            fp->scroll = (uint32_t)parse_i(&a);
            CALL(FilePane_ensure_visible, fp);
            sc_put("V cur="); sc_num((int32_t)fp->cursor); sc_put(" scr="); sc_num((int32_t)fp->scroll); sc_put("\n");
            break;
        }
        case 'G': {
            fp->cursor = (uint32_t)parse_i(&a);
            FileEntry *e = CALL(FilePane_get_selected_entry, fp);
            sc_put("G ent="); sc_put(e ? (m->name_pool + e->name_off) : "null"); sc_put("\n");
            break;
        }
        case 'R': {
            g_reload_shift = (int)parse_i(&a);
            while (*a == ' ') a++;
            const char *fn = (a[0] == '-' && !a[1]) ? nullptr : a;
            CALL(FilePane_reload_keep_cursor, fp, fn);
            sc_put("R cur="); sc_num((int32_t)fp->cursor); sc_put(" scr="); sc_num((int32_t)fp->scroll); sc_put("\n");
            break;
        }
        case 'P': {
            g_exit_virt = (int)parse_i(&a);
            g_chdir_err = (intptr_t)parse_i(&a);
            m->du_mode = (int32_t)parse_i(&a);
            while (*a == ' ') a++;
            (void)str_copy(m->path, a, PATH_MAX);
            m->count = 4;
            fp->base.h = 10;
            fp->cursor = 3;
            fp->scroll = 1;
            CALL(FilePane_enter_parent, fp);
            sc_put("P path=|"); sc_esc(m->path); sc_put("| cur="); sc_num((int32_t)fp->cursor);
            sc_put(" scr="); sc_num((int32_t)fp->scroll); sc_put("\n");
            break;
        }
        default: continue;
        }
        sc_flush();
    }
    DUOP_DELETE(fp);
    sc_flush();
    return 0;
}
