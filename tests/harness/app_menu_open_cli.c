/* tests/harness/app_menu_open_cli.c - A2.1 differential harness for app_menu.c and app_open.c. */
#include "duop_app.h"
#include "duop_app_int.h"
#include "duop_classes.h"
#include "duop_config.h"
#include "duop_fs.h"
#include "duop_oop.h"
#include "duop_pal.h"
#include "duop_pkg.h"
#include "duop_string.h"
#include "duop_sys.h"
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

/* ---- shared fake state ---- */
static App g_app;
static FilePane g_lp, g_rp;
static DirModel g_lm, g_rm;
static ScreenBuffer g_scr;
static FileEntry g_fe;
static int g_has_fe;
static char g_fe_name[256];

static int g_ensure_rc;
static int g_bg_rc;
static int g_has_marked;
static int g_build_argc;
static char g_marked_path[128];

static int g_vfs_par_rc;
static int g_vfs_load_rc;
static intptr_t g_nav_rc;
static const char *g_open_rule;

const LangStrings *i18n_get(void) { return &lang_en_table; }

FilePane *get_inactive_pane(App *app)
{
    return (FilePane *)(uintptr_t)app->active_pane == &g_lp ? &g_rp : &g_lp;
}

/* ---- action dispatch stubs ---- */
#define STUB_CMD(name) void name(App *app) { sc_put("call " #name " "); sc_num(app != nullptr); sc_nl(); }
STUB_CMD(App_cmd_quit)
STUB_CMD(App_cmd_help)
STUB_CMD(App_cmd_rename)
STUB_CMD(App_cmd_view)
STUB_CMD(App_cmd_edit)
STUB_CMD(App_cmd_copy)
STUB_CMD(App_cmd_move)
STUB_CMD(App_cmd_mkdir)
STUB_CMD(App_cmd_delete)
STUB_CMD(App_cmd_toggle_preview)
STUB_CMD(App_cmd_toggle_hidden)
STUB_CMD(App_cmd_cycle_theme)
STUB_CMD(App_cmd_toggle_lang)
STUB_CMD(App_cmd_start_filter)
STUB_CMD(App_cmd_goto)
STUB_CMD(App_cmd_newfile)
STUB_CMD(App_cmd_shell)

void App_sync_preview(App *app) { sc_put("call App_sync_preview "); sc_num(app != nullptr); sc_nl(); }

void App_set_status(App *app, const char *msg, int kind)
{
    sc_put("call App_set_status "); sc_num(app != nullptr); sc_put(" k="); sc_num(kind);
    sc_put(" m=|"); sc_put(msg ? msg : "(null)"); sc_put("|\n");
}

void App_run_expanded_cmd(App *app, const char *tmpl, const char *cur_name, const char *cur_path)
{
    sc_put("call App_run_expanded_cmd "); sc_num(app != nullptr);
    sc_put(" t=|"); sc_put(tmpl ? tmpl : "-");
    sc_put("| n=|"); sc_put(cur_name ? cur_name : "-");
    sc_put("| p=|"); sc_put(cur_path ? cur_path : "-"); sc_put("|\n");
}

UserMenuDialog *UserMenuDialog_new(App *app)
{
    sc_put("call UserMenuDialog_new "); sc_num(app != nullptr); sc_nl();
    return (UserMenuDialog *)(uintptr_t)0x3001;
}

ViewerDialog *ViewerDialog_new(App *app, const char *path)
{
    sc_put("call ViewerDialog_new "); sc_num(app != nullptr); sc_put(" p=|"); sc_put(path ? path : ""); sc_put("|\n");
    return (ViewerDialog *)(uintptr_t)0x3002;
}

const char *fs_errno_message(int neg_errno)
{
    if (neg_errno == -ENOENT) return "E_NOENT";
    if (neg_errno == -EACCES) return "E_ACCES";
    if (neg_errno == -EIO) return "E_IO";
    if (neg_errno == -ENOMEM) return "E_NOMEM";
    return "E_OTHER";
}

FileEntry *fake_FilePane_get_selected_entry(FilePane *fp)
{
    (void)fp;
    return g_has_fe ? &g_fe : nullptr;
}

const char *fake_DirModel_entry_name(const DirModel *m, const FileEntry *e)
{
    (void)m; (void)e;
    return g_fe_name;
}

void fake_FilePane_reload_keep_cursor(FilePane *fp, const char *focus)
{
    sc_put("call FilePane_reload_keep_cursor p="); sc_num(fp == &g_lp ? 1 : 2);
    sc_put(" f=|"); sc_put(focus ? focus : "-"); sc_put("|\n");
}

void fake_FilePane_enter_parent(FilePane *fp)
{
    sc_put("call FilePane_enter_parent p="); sc_num(fp == &g_lp ? 1 : 2); sc_nl();
}

void fake_FilePane_ensure_visible(FilePane *fp)
{
    sc_put("call FilePane_ensure_visible p="); sc_num(fp == &g_lp ? 1 : 2); sc_nl();
}

intptr_t fake_FilePane_nav_to(FilePane *fp, const char *path)
{
    sc_put("call FilePane_nav_to p="); sc_num(fp == &g_lp ? 1 : 2);
    sc_put(" to=|"); sc_put(path ? path : ""); sc_put("|\n");
    return g_nav_rc;
}

int fake_pkg_ensure_exec(const char *path)
{
    sc_put("call pkg_ensure_exec |"); sc_put(path ? path : ""); sc_put("|\n");
    return g_ensure_rc;
}

int fake_pkg_action_has_marked(PkgAction *act) { (void)act; return g_has_marked; }

int fake_pkg_write_marked_list(DirModel *m, const char *fallback, char *out_tmp, int cap)
{
    (void)m;
    sc_put("call pkg_write_marked_list fb=|"); sc_put(fallback ? fallback : ""); sc_put("|\n");
    (void)str_copy(out_tmp, g_marked_path, (size_t)cap);
    intptr_t fd = os_openat(AT_FDCWD, out_tmp, O_WRONLY | O_CREAT | O_NOFOLLOW, 0600);
    if (fd >= 0) (void)os_close((int)fd);
    return 0;
}

int fake_pkg_action_build_argv(PkgAction *act, const char **ctx, char *str_buf, int cap, char **argv_out, int max_args)
{
    (void)act; (void)cap; (void)max_args;
    sc_put("call pkg_action_build_argv d0=|"); sc_put(ctx[0] ? ctx[0] : "-");
    sc_put("| d1=|"); sc_put(ctx[1] ? ctx[1] : "-");
    sc_put("| n=|"); sc_put(ctx[2] ? ctx[2] : "-");
    sc_put("| p=|"); sc_put(ctx[3] ? ctx[3] : "-");
    sc_put("| mk="); sc_num(ctx[4] != nullptr); sc_nl();
    if (g_build_argc <= 0) return g_build_argc;
    (void)str_copy(str_buf, "/bin/fake_tool", 32);
    argv_out[0] = str_buf;
    argv_out[1] = nullptr;
    return 1;
}

int fake_sys_exec_argv(const char *exe, char **argv, char **envp)
{
    char cwd[PATH_MAX];
    if (os_getcwd(cwd, sizeof cwd) < 0) cwd[0] = '\0';
    sc_put("call sys_exec_argv |"); sc_put(exe); sc_put("| a0=|"); sc_put(argv[0]);
    sc_put("| env="); sc_num(envp != nullptr);
    sc_put(" in_act="); sc_num(str_cmp(cwd, g_lm.path) == 0); sc_nl();
    return 0;
}

int fake_sys_exec_argv_bg(const char *exe, char **argv, char **envp)
{
    char cwd[PATH_MAX];
    if (os_getcwd(cwd, sizeof cwd) < 0) cwd[0] = '\0';
    sc_put("call sys_exec_argv_bg |"); sc_put(exe); sc_put("| a0=|"); sc_put(argv[0]);
    sc_put("| env="); sc_num(envp != nullptr);
    sc_put(" in_act="); sc_num(str_cmp(cwd, g_lm.path) == 0); sc_nl();
    return g_bg_rc;
}

int fake_pkg_vfs_parent_uri(const char *uri, char *out, int cap)
{
    sc_put("call pkg_vfs_parent_uri |"); sc_put(uri); sc_put("|\n");
    if (!g_vfs_par_rc) return 0;
    (void)str_copy(out, "vfs://parent", (size_t)cap);
    return 1;
}

int fake_pkg_vfs_load_dir(char **envp, DirModel *m, const char *uri)
{
    sc_put("call pkg_vfs_load_dir env="); sc_num(envp != nullptr); sc_put(" m="); sc_num(m != nullptr);
    sc_put(" u=|"); sc_put(uri); sc_put("|\n");
    return g_vfs_load_rc;
}

void fake_app_jump_virtual_entry(App *app, FilePane *fp, const char *full)
{
    sc_put("call app_jump_virtual_entry "); sc_num(app != nullptr); sc_put(" p="); sc_num(fp == &g_lp ? 1 : 2);
    sc_put(" |"); sc_put(full); sc_put("|\n");
}

const char *fake_config_lookup_open(const char *name)
{
    sc_put("call config_lookup_open |"); sc_put(name); sc_put("|\n");
    return g_open_rule;
}

/* ---- include C modules under test ---- */
#define FilePane_get_selected_entry fake_FilePane_get_selected_entry
#define DirModel_entry_name fake_DirModel_entry_name
#define FilePane_reload_keep_cursor fake_FilePane_reload_keep_cursor
#define FilePane_enter_parent fake_FilePane_enter_parent
#define FilePane_ensure_visible fake_FilePane_ensure_visible
#define FilePane_nav_to fake_FilePane_nav_to
#define pkg_ensure_exec fake_pkg_ensure_exec
#define pkg_action_has_marked fake_pkg_action_has_marked
#define pkg_write_marked_list fake_pkg_write_marked_list
#define pkg_action_build_argv fake_pkg_action_build_argv
#define sys_exec_argv fake_sys_exec_argv
#define sys_exec_argv_bg fake_sys_exec_argv_bg
#define pkg_vfs_parent_uri fake_pkg_vfs_parent_uri
#define pkg_vfs_load_dir fake_pkg_vfs_load_dir
#define app_jump_virtual_entry fake_app_jump_virtual_entry
#define config_lookup_open fake_config_lookup_open
#include OPEN_SRC
#include MENU_SRC
#undef FilePane_get_selected_entry
#undef DirModel_entry_name
#undef FilePane_reload_keep_cursor
#undef FilePane_enter_parent
#undef FilePane_ensure_visible
#undef FilePane_nav_to
#undef pkg_ensure_exec
#undef pkg_action_has_marked
#undef pkg_write_marked_list
#undef pkg_action_build_argv
#undef sys_exec_argv
#undef sys_exec_argv_bg
#undef pkg_vfs_parent_uri
#undef pkg_vfs_load_dir
#undef app_jump_virtual_entry
#undef config_lookup_open

#ifdef __x86_64__
void ref_App_cmd_usermenu(App *app);
void ref_App_dispatch_action(App *app, int act_id);
void ref_App_exec_menu_tmpl(App *app, const char *tmpl);
void ref_App_run_pkg_action(App *app, PkgAction *act);
void ref_App_open_selected_entry(App *app, FilePane *pane);
void ref_App_reload_panes(App *app, const char *focus);
void ref_reload_both_panes(App *app, const char *focus);
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
    mem_zero(&g_scr, sizeof g_scr);
    mem_zero(&g_fe, sizeof g_fe);
    static char *fake_env[] = { "HOME=/tmp", nullptr };
    g_app.envp = (uint64_t)(uintptr_t)fake_env;
    g_app.left_pane = (uint64_t)(uintptr_t)&g_lp;
    g_app.right_pane = (uint64_t)(uintptr_t)&g_rp;
    g_app.active_pane = (uint64_t)(uintptr_t)&g_lp;
    g_app.screen = (uint64_t)(uintptr_t)&g_scr;
    g_lp.model = (uint64_t)(uintptr_t)&g_lm;
    g_rp.model = (uint64_t)(uintptr_t)&g_rm;
    (void)str_copy(g_lm.path, "/act", sizeof g_lm.path);
    (void)str_copy(g_rm.path, "/inact", sizeof g_rm.path);
    g_has_fe = 0; g_fe_name[0] = '\0';
    g_ensure_rc = 0; g_bg_rc = 0; g_has_marked = 0; g_build_argc = 1;
    g_vfs_par_rc = 0; g_vfs_load_rc = 0; g_nav_rc = 0; g_open_rule = nullptr;
}

static void handle_menu(char *p)
{
    int sub = (int)sc_tok_i64(&p);
    if (sub == 0) {
        init_state();
        CALL(App_cmd_usermenu, &g_app);
        sc_put("UM d="); sc_num((int64_t)g_app.active_dialog); sc_put(" rd="); sc_num(g_app.need_redraw); sc_nl();
    } else if (sub == 1) {
        init_state();
        int id = (int)sc_tok_i64(&p);
        CALL(App_dispatch_action, &g_app, id);
    } else if (sub == 2) {
        /* M 2 <has_fe> <long_path> <name> <tmpl> */
        init_state();
        g_has_fe = (int)sc_tok_i64(&p);
        int lp = (int)sc_tok_i64(&p);
        sc_tok_str(&p, g_fe_name, sizeof g_fe_name);
        char tmpl[64]; sc_tok_str(&p, tmpl, sizeof tmpl);
        if (lp) { for (int i = 0; i < 4090; i++) g_lm.path[i] = 'a'; g_lm.path[4090] = '\0'; }
        CALL(App_exec_menu_tmpl, &g_app, tmpl);
    } else if (sub == 3) {
        /* M 3 <has_fe> <long_path> <ensure_rc> <has_mk> <build_argc> <bg_rc> <mode> <menu_lbl> <dir> <mk_file> */
        init_state();
        g_has_fe = (int)sc_tok_i64(&p);
        int lp = (int)sc_tok_i64(&p);
        g_ensure_rc = (int)sc_tok_i64(&p);
        g_has_marked = (int)sc_tok_i64(&p);
        g_build_argc = (int)sc_tok_i64(&p);
        g_bg_rc = (int)sc_tok_i64(&p);
        char mode[16], mlbl[64], dir[256];
        sc_tok_str(&p, mode, sizeof mode);
        sc_tok_str(&p, mlbl, sizeof mlbl);
        sc_tok_str(&p, dir, sizeof dir);
        sc_tok_str(&p, g_marked_path, sizeof g_marked_path);
        (void)str_copy(g_lm.path, dir, sizeof g_lm.path);
        if (lp) { for (int i = 0; i < 4090; i++) g_lm.path[i] = 'a'; g_lm.path[4090] = '\0'; }
        (void)str_copy(g_fe_name, "item.txt", sizeof g_fe_name);
        PkgAction act; mem_zero(&act, sizeof act);
        (void)str_copy(act.name, "pkg_name", sizeof act.name);
        if (!(mlbl[0] == '-' && mlbl[1] == '\0')) (void)str_copy(act.menu, mlbl, sizeof act.menu);
        if (!(mode[0] == '-' && mode[1] == '\0')) (void)str_copy(act.mode, mode, sizeof act.mode);
        char before[PATH_MAX], after[PATH_MAX];
        if (os_getcwd(before, sizeof before) < 0) before[0] = '\0';
        CALL(App_run_pkg_action, &g_app, &act);
        if (os_getcwd(after, sizeof after) < 0) after[0] = '\0';
        FileStat st;
        int mk_exists = (g_has_marked && os_statat(AT_FDCWD, g_marked_path, &st, 0) == 0);
        sc_put("PA ff="); sc_num(g_scr.force_full); sc_put(" cwd="); sc_num(str_cmp(before, after) == 0);
        sc_put(" mk_left="); sc_num(mk_exists); sc_nl();
    }
}

static int g_arc_open_rc;
/* the archive layer's answer to "enter this entry as an archive folder" (the frozen asm never asks: the default 0 keeps the differential exact) */
int arc_open_entry(App *app, FilePane *pane);
int arc_open_entry(App *app, FilePane *pane) { (void)app; (void)pane; return g_arc_open_rc; }

static void handle_open(char *p)
{
    int sub = (int)sc_tok_i64(&p);
    if (sub == 2) {
        /* O 2 <arc_open_rc>: a plain file under the cursor (C only): when the archive layer handles it no viewer may open */
        init_state();
        g_has_fe = 1;
        g_fe.flags = 0;
        g_lm.virtual_mode = 0;
        (void)str_copy(g_fe_name, "a.zip", sizeof g_fe_name);
        g_open_rule = nullptr;
        g_arc_open_rc = (int)sc_tok_i64(&p);
        App_open_selected_entry(&g_app, &g_lp);
        sc_put("OA dlg="); sc_num(g_app.active_dialog != 0); sc_nl();
        g_arc_open_rc = 0;
        return;
    }
    if (sub == 0) {
        /* O 0 <has_fe> <flags> <vmode> <vfs_par> <vfs_load_rc> <nav_rc> <has_arc_ctx> <has_rule> <name> */
        init_state();
        g_has_fe = (int)sc_tok_i64(&p);
        g_fe.flags = (uint32_t)sc_tok_i64(&p);
        g_lm.virtual_mode = (uint32_t)sc_tok_i64(&p);
        g_vfs_par_rc = (int)sc_tok_i64(&p);
        g_vfs_load_rc = (int)sc_tok_i64(&p);
        g_nav_rc = sc_tok_i64(&p);
        int has_arc = (int)sc_tok_i64(&p);
        int has_rule = (int)sc_tok_i64(&p);
        sc_tok_str(&p, g_fe_name, sizeof g_fe_name);
        if (g_lm.virtual_mode == 3) (void)str_copy(g_lm.path, "vfs://root/sub", sizeof g_lm.path);
        if (has_arc) {
            char *saved = mem_alloc(32);
            if (saved) { (void)str_copy(saved, "/saved/path", 32); g_lm.arc_ctx = saved; }
        }
        g_open_rule = has_rule ? "open:%f" : nullptr;
        g_lp.cursor = 7; g_lp.scroll = 3;
        CALL(App_open_selected_entry, &g_app, &g_lp);
        sc_put("OP vm="); sc_num(g_lm.virtual_mode); sc_put(" arc="); sc_num(g_lm.arc_ctx != 0);
        sc_put(" cur="); sc_num(g_lp.cursor); sc_put(" scr="); sc_num(g_lp.scroll);
        sc_put(" dlg="); sc_num((int64_t)g_app.active_dialog); sc_put(" rd="); sc_num(g_app.need_redraw); sc_nl();
        if (g_lm.arc_ctx) { mem_free(g_lm.arc_ctx); g_lm.arc_ctx = nullptr; }
    } else if (sub == 1) {
        /* O 1 <use_alias> <focus_or_-> */
        init_state();
        int alias = (int)sc_tok_i64(&p);
        char foc[64]; sc_tok_str(&p, foc, sizeof foc);
        const char *f = (foc[0] == '-' && foc[1] == '\0') ? nullptr : foc;
        if (alias) CALL(reload_both_panes, &g_app, f);
        else CALL(App_reload_panes, &g_app, f);
        sc_put("RL rd="); sc_num(g_app.need_redraw); sc_nl();
    }
}

int duop_main(int argc, char **argv, char **envp, const uintptr_t *auxv)
{
    (void)envp; (void)auxv;
    g_use_ref = (argc >= 2 && argv[1][0] == 'r');
    char *script = sc_read_all();
    for (char *line; (line = sc_next_line(&script));) {
        char *p = line;
        char tag = *p++;
        if (tag == 'M') handle_menu(p);
        else if (tag == 'O') handle_open(p);
    }
    sc_flush();
    return 0;
}
