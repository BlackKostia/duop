/* tests/harness/app_ext_cli.c - A2.4 differential harness for src/app/app_ext.c. */
#include "duop_app.h"
#include "duop_app_int.h"
#include "duop_classes.h"
#include "duop_config.h"
#include "duop_fs.h"
#include "duop_keys.h"
#include "duop_oop.h"
#include "duop_pkg.h"
#include "duop_string.h"
#include "duop_tui.h"
#include "duop_ui.h"
#include "script_cli.h"

static void sc_nl(void) { sc_put("\n"); }

static int64_t sc_tok_i64(char **pp)
{
    char *p = *pp;
    while (*p == ' ') p++;
    int neg = (*p == '-');
    if (neg) p++;
    int64_t v = 0;
    while (*p >= '0' && *p <= '9') v = v * 10 + (*p++ - '0');
    *pp = p;
    return neg ? -v : v;
}

static void sc_tok_str(char **pp, char *dst, size_t cap)
{
    char *p = *pp;
    while (*p == ' ') p++;
    size_t i = 0;
    while (*p && *p != ' ') {
        if (i + 1 < cap) dst[i++] = *p;
        p++;
    }
    dst[i] = '\0';
    *pp = p;
}

static App g_app;
static FilePane g_lp, g_rp;
static DirModel g_lm, g_rm;

static intptr_t g_nav_rc;
static intptr_t g_find_idx;
static uintptr_t g_fuz_sel;
static int g_cfg_cnt;
static int g_cfg_item_ok;
static int g_pkg_ok;
static const char *g_bm_sel;
static int g_srch_rc;
static int g_hist_rc;
static int g_arc_rc;
static int g_diff_cnt;
static int g_dlg_oom;
static PkgAction g_pkg_act;

const LangStrings *i18n_get(void) { return &lang_en_table; }

intptr_t fake_FilePane_nav_to(FilePane *fp, const char *p)
{
    sc_put("call FilePane_nav_to p="); sc_num(fp == &g_lp ? 1 : 2);
    sc_put(" |"); sc_put(p ? p : "-"); sc_put("|\n");
    return g_nav_rc;
}

void fake_FilePane_ensure_visible(FilePane *fp)
{
    sc_put("call FilePane_ensure_visible p="); sc_num(fp == &g_lp ? 1 : 2); sc_nl();
}

int fake_FilePane_hist_back(FilePane *fp) { (void)fp; sc_put("call FilePane_hist_back\n"); return g_hist_rc; }
int fake_FilePane_hist_fwd(FilePane *fp) { (void)fp; sc_put("call FilePane_hist_fwd\n"); return g_hist_rc; }

intptr_t fake_DirModel_find_by_name(DirModel *m, const char *name)
{
    (void)m;
    sc_put("call DirModel_find_by_name |"); sc_put(name ? name : "-"); sc_put("|\n");
    return g_find_idx;
}

void fake_DirModel_sort(DirModel *m) { sc_put("call DirModel_sort "); sc_num(m != nullptr); sc_nl(); }

int fake_dir_search_run(DirModel *m, const char *root, const char *mask, const char *grep)
{
    (void)m;
    sc_put("call dir_search_run r=|"); sc_put(root); sc_put("| m=|"); sc_put(mask);
    sc_put("| g=|"); sc_put(grep); sc_put("|\n");
    return g_srch_rc;
}

int fake_dir_diff_mark(DirModel *a, DirModel *b)
{
    sc_put("call dir_diff_mark a="); sc_num(a == &g_lm ? 1 : 2);
    sc_put(" b="); sc_num(b == &g_lm ? 1 : 2); sc_nl();
    return g_diff_cnt;
}

SearchDialog *SearchDialog_new(App *app)
{
    sc_put("call SearchDialog_new "); sc_num(app != nullptr); sc_nl();
    return g_dlg_oom ? nullptr : (SearchDialog *)(uintptr_t)0x7001;
}

FuzzyDialog *FuzzyDialog_new(App *app, int mode, const char *base)
{
    sc_put("call FuzzyDialog_new "); sc_num(app != nullptr); sc_put(" m="); sc_num(mode);
    sc_put(" b=|"); sc_put(base); sc_put("|\n");
    return g_dlg_oom ? nullptr : (FuzzyDialog *)(uintptr_t)(0x7100 + mode);
}

uintptr_t fake_FuzzyDialog_get_selected(FuzzyDialog *d) { (void)d; return g_fuz_sel; }
const char *fake_BookmarkDialog_get_selected(BookmarkDialog *d) { (void)d; return g_bm_sel; }

BulkRenameDialog *BulkRenameDialog_new(App *app, FilePane *fp)
{
    sc_put("call BulkRenameDialog_new "); sc_num(app != nullptr); sc_put(" p="); sc_num(fp == &g_lp ? 1 : 2); sc_nl();
    return g_dlg_oom ? nullptr : (BulkRenameDialog *)(uintptr_t)0x7002;
}

int fake_config_menu_count(void) { return g_cfg_cnt; }
int fake_config_menu_item(int idx, const char **lbl, const char **cmd)
{
    sc_put("call config_menu_item idx="); sc_num(idx); sc_nl();
    if (!g_cfg_item_ok) return 0;
    *lbl = "LBL"; *cmd = "echo_cmd";
    return 1;
}
PkgAction *fake_pkg_menu_get(int idx)
{
    sc_put("call pkg_menu_get idx="); sc_num(idx); sc_nl();
    return g_pkg_ok ? &g_pkg_act : nullptr;
}

void App_task_conflict_done(App *app, int res, ConflictDialog *d)
{
    sc_put("call App_task_conflict_done "); sc_num(app != nullptr); sc_put(" r="); sc_num(res);
    sc_put(" d="); sc_num(d != nullptr); sc_nl();
}
void App_exec_menu_tmpl(App *app, const char *t)
{
    sc_put("call App_exec_menu_tmpl "); sc_num(app != nullptr); sc_put(" |"); sc_put(t); sc_put("|\n");
}
void App_run_pkg_action(App *app, PkgAction *a)
{
    sc_put("call App_run_pkg_action "); sc_num(app != nullptr); sc_put(" a="); sc_num(a == &g_pkg_act); sc_nl();
}
void App_sync_preview(App *app) { sc_put("call App_sync_preview "); sc_num(app != nullptr); sc_nl(); }
void App_set_status(App *app, const char *m, int k)
{
    sc_put("call App_set_status "); sc_num(app != nullptr); sc_put(" k="); sc_num(k);
    sc_put(" m=|"); sc_put(m ? m : "-"); sc_put("|\n");
}
void App_apply_chmod(App *app, ChmodDialog *d) { sc_put("call App_apply_chmod "); sc_num(app != nullptr && d != nullptr); sc_nl(); }
void App_apply_link(App *app, InputDialog *d, int hard)
{
    sc_put("call App_apply_link "); sc_num(app != nullptr && d != nullptr); sc_put(" h="); sc_num(hard); sc_nl();
}
void App_view_as_done(App *app, ViewAsDialog *d) { sc_put("call App_view_as_done "); sc_num(app != nullptr && d != nullptr); sc_nl(); }
void App_cmd_unpack(App *app) { sc_put("call App_cmd_unpack "); sc_num(app != nullptr); sc_nl(); }
void App_cmd_arctest(App *app) { sc_put("call App_cmd_arctest "); sc_num(app != nullptr); sc_nl(); }
void App_unpack_confirm(App *app, const char *dst) { sc_put("call App_unpack_confirm "); sc_num(app != nullptr); sc_put(" d=|"); sc_put(dst); sc_put("|"); sc_nl(); }
void App_cmd_undo(App *app) { sc_put("call App_cmd_undo "); sc_num(app != nullptr); sc_nl(); }
void App_cmd_chmod(App *app) { sc_put("call App_cmd_chmod "); sc_num(app != nullptr); sc_nl(); }
void App_cmd_info(App *app) { sc_put("call App_cmd_info "); sc_num(app != nullptr); sc_nl(); }
void App_cmd_link(App *app, int hard) { sc_put("call App_cmd_link "); sc_num(app != nullptr); sc_put(" h="); sc_num(hard); sc_nl(); }
void App_dispatch_action(App *app, int id) { sc_put("call App_dispatch_action "); sc_num(app != nullptr); sc_put(" id="); sc_num(id); sc_nl(); }
int App_cmd_view_mode(App *app) { sc_put("call App_cmd_view_mode "); sc_num(app != nullptr); sc_nl(); return 1; }
int App_cmd_packages(App *app) { sc_put("call App_cmd_packages "); sc_num(app != nullptr); sc_nl(); return 1; }
void du_toggle(App *app) { sc_put("call du_toggle "); sc_num(app != nullptr); sc_nl(); }
int arc_handle_enter(App *app) { sc_put("call arc_handle_enter "); sc_num(app != nullptr); sc_nl(); return g_arc_rc; }
void term_toggle(App *app) { sc_put("call term_toggle "); sc_num(app != nullptr); sc_nl(); }
void App_layout(App *app) { sc_put("call App_layout "); sc_num(app != nullptr); sc_nl(); }

#define FilePane_nav_to fake_FilePane_nav_to
#define FilePane_ensure_visible fake_FilePane_ensure_visible
#define FilePane_hist_back fake_FilePane_hist_back
#define FilePane_hist_fwd fake_FilePane_hist_fwd
#define DirModel_find_by_name fake_DirModel_find_by_name
#define DirModel_sort fake_DirModel_sort
#define dir_search_run fake_dir_search_run
#define dir_diff_mark fake_dir_diff_mark
#define FuzzyDialog_get_selected fake_FuzzyDialog_get_selected
#define BookmarkDialog_get_selected fake_BookmarkDialog_get_selected
#define config_menu_count fake_config_menu_count
#define config_menu_item fake_config_menu_item
#define pkg_menu_get fake_pkg_menu_get
#include EXT_SRC
#undef FilePane_nav_to
#undef FilePane_ensure_visible
#undef FilePane_hist_back
#undef FilePane_hist_fwd
#undef DirModel_find_by_name
#undef DirModel_sort
#undef dir_search_run
#undef dir_diff_mark
#undef FuzzyDialog_get_selected
#undef BookmarkDialog_get_selected
#undef config_menu_count
#undef config_menu_item
#undef pkg_menu_get

#ifdef __x86_64__
void ref_app_jump_virtual_entry(App *app, FilePane *pane, char *full_path);
intptr_t ref_App_close_dialog_ext(App *app, Dialog *dlg, FilePane *pane, DirModel *model, int dlg_res);
int ref_app_handle_ext_key(App *app, int key, int mods);
#endif

static int g_use_ref;
#ifdef __x86_64__
#define CALL(fn, ...) (g_use_ref ? ref_##fn(__VA_ARGS__) : fn(__VA_ARGS__))
#else
#define CALL(fn, ...) fn(__VA_ARGS__)
#endif

static void init_state(void)
{
    mem_zero(&g_app, sizeof g_app);
    mem_zero(&g_lp, sizeof g_lp);
    mem_zero(&g_rp, sizeof g_rp);
    mem_zero(&g_lm, sizeof g_lm);
    mem_zero(&g_rm, sizeof g_rm);
    g_app.left_pane = (uint64_t)(uintptr_t)&g_lp;
    g_app.right_pane = (uint64_t)(uintptr_t)&g_rp;
    g_app.active_pane = (uint64_t)(uintptr_t)&g_lp;
    g_lp.model = (uint64_t)(uintptr_t)&g_lm;
    g_rp.model = (uint64_t)(uintptr_t)&g_rm;
    (void)str_copy(g_lm.path, "/left", sizeof g_lm.path);
    (void)str_copy(g_rm.path, "/right", sizeof g_rm.path);
    g_nav_rc = 0; g_find_idx = 3; g_fuz_sel = 0; g_cfg_cnt = 2; g_cfg_item_ok = 1;
    g_pkg_ok = 1; g_bm_sel = nullptr; g_srch_rc = 0; g_hist_rc = 1; g_arc_rc = 1;
    g_diff_cnt = 4; g_dlg_oom = 0;
}

int duop_main(int argc, char **argv, char **envp, const uintptr_t *auxv)
{
    (void)envp; (void)auxv;
    g_use_ref = (argc >= 2 && argv[1][0] == 'r');
    char *script = sc_read_all();
    for (char *line; (line = sc_next_line(&script));) {
        char *p = line;
        char tag = *p++;
        init_state();
        if (tag == 'J') {
            /* J <nav_rc> <find_idx> <full_path> */
            g_nav_rc = sc_tok_i64(&p);
            g_find_idx = sc_tok_i64(&p);
            char fp[256]; sc_tok_str(&p, fp, sizeof fp);
            g_lm.virtual_mode = 1;
            g_lp.cursor = 0;
            CALL(app_jump_virtual_entry, &g_app, &g_lp, fp);
            sc_put("JV vm="); sc_num(g_lm.virtual_mode); sc_put(" cur="); sc_num(g_lp.cursor);
            sc_put(" rd="); sc_num(g_app.need_redraw); sc_nl();
        } else if (tag == 'D') {
            /* D <op_tag> <arg1> <arg2> <arg3> */
            static union { Dialog d; UserMenuDialog um; FuzzyDialog fz; SearchDialog sd; InputDialog id; } u;
            mem_zero(&u, sizeof u);
            uint32_t op = (uint32_t)sc_tok_i64(&p);
            int64_t a1 = sc_tok_i64(&p), a2 = sc_tok_i64(&p), a3 = sc_tok_i64(&p);
            u.d.op_tag = op;
            if (op == OP_UNPACK) (void)str_copy((char *)u.id.buf, "/dest/dir", sizeof u.id.buf);
            else if (op == OP_USERMENU) { u.um.cursor = (uint32_t)a1; g_cfg_item_ok = (int)a2; g_pkg_ok = (int)a3; }
            else if (op == OP_BOOKMARKS || op == OP_FRECENCY) { g_bm_sel = a1 ? "/bm/dir" : nullptr; g_nav_rc = a2; }
            else if (op == OP_PALETTE) { g_fuz_sel = (uintptr_t)a1; g_pkg_ok = (int)a2; }
            else if (op == OP_FUZZY) {
                (void)str_copy((char *)u.fz.base_path, "/base", sizeof u.fz.base_path);
                g_fuz_sel = a1 ? (uintptr_t)"sub/hit.c" : 0;
            } else if (op == OP_SEARCH) {
                (void)str_copy((char *)u.sd.name_buf, "*.c", sizeof u.sd.name_buf);
                (void)str_copy((char *)u.sd.grep_buf, "main", sizeof u.sd.grep_buf);
                g_srch_rc = (int)a1; g_lm.has_dotdot = (uint32_t)a2; g_lm.count = (uint64_t)a3;
            }
            intptr_t rc = CALL(App_close_dialog_ext, &g_app, &u.d, &g_lp, &g_lm, (int)a1);
            sc_put("DE rc="); sc_num(rc); sc_put(" cur="); sc_num(g_lp.cursor);
            sc_put(" scr="); sc_num(g_lp.scroll); sc_put(" rd="); sc_num(g_app.need_redraw); sc_nl();
        } else if (tag == 'K') {
            /* K <key> <mods> <active_right> <aux> */
            int key = (int)sc_tok_i64(&p);
            int mods = (int)sc_tok_i64(&p);
            int right = (int)sc_tok_i64(&p);
            int aux = (int)sc_tok_i64(&p);
            if (right) g_app.active_pane = (uint64_t)(uintptr_t)&g_rp;
            g_hist_rc = aux; g_arc_rc = aux; g_dlg_oom = aux;
            int rc = CALL(app_handle_ext_key, &g_app, key, mods);
            sc_put("EK rc="); sc_num(rc); sc_put(" d="); sc_num((int64_t)g_app.active_dialog);
            sc_put(" rd="); sc_num(g_app.need_redraw); sc_nl();
        }
    }
    sc_flush();
    return 0;
}
