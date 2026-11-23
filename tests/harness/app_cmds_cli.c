/* tests/harness/app_cmds_cli.c - A2.2 differential harness for src/app/app_cmds.c. */
#include "duop_app.h"
#include "duop_app_int.h"
#include "duop_classes.h"
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

static App g_app;
static FilePane g_lp, g_rp;
static DirModel g_lm, g_rm;
static ScreenBuffer g_scr;
static FileEntry g_fe;
static int g_has_fe;
static size_t g_sel_cnt;
static char g_fe_name[128];
static int g_arc_rc;
static int g_vfs_rc;

const LangStrings *i18n_get(void) { return &lang_en_table; }

FilePane *get_inactive_pane(App *app)
{
    return (FilePane *)(uintptr_t)app->active_pane == &g_lp ? &g_rp : &g_lp;
}

const char *fs_errno_message(int neg_errno)
{
    if (neg_errno == -ENAMETOOLONG) return "E_NAMETOOLONG";
    return "E_OTHER";
}

void App_set_status(App *app, const char *msg, int kind)
{
    sc_put("call App_set_status "); sc_num(app != nullptr); sc_put(" k="); sc_num(kind);
    sc_put(" m=|"); sc_put(msg ? msg : "(null)"); sc_put("|\n");
}

void reload_both_panes(App *app, const char *focus)
{
    sc_put("call reload_both_panes "); sc_num(app != nullptr); sc_put(" f=|"); sc_put(focus ? focus : "-"); sc_put("|\n");
}

void App_open_viewer(App *app, const char *path)
{
    sc_put("call App_open_viewer "); sc_num(app != nullptr); sc_put(" p=|"); sc_put(path ? path : ""); sc_put("|\n");
}

InputDialog *InputDialog_new(App *app, const char *title, const char *prompt, const char *init, uint32_t tag)
{
    sc_put("call InputDialog_new "); sc_num(app != nullptr);
    sc_put(" t=|"); sc_put(title ? title : "-");
    sc_put("| p=|"); sc_put(prompt ? prompt : "-");
    sc_put("| i=|"); sc_put(init ? init : "-");
    sc_put("| tag="); sc_num(tag); sc_nl();
    return (InputDialog *)(uintptr_t)(0x4000 + tag);
}

ConfirmDialog *ConfirmDialog_new(App *app, const char *title, const char *msg, const char *detail, uint32_t tag)
{
    sc_put("call ConfirmDialog_new "); sc_num(app != nullptr);
    sc_put(" t=|"); sc_put(title ? title : "-");
    sc_put("| m=|"); sc_put(msg ? msg : "-");
    sc_put("| d=|"); sc_put(detail ? detail : "-");
    sc_put("| tag="); sc_num(tag); sc_nl();
    return (ConfirmDialog *)(uintptr_t)(0x5000 + tag);
}

FileEntry *fake_FilePane_get_selected_entry(FilePane *fp) { (void)fp; return g_has_fe ? &g_fe : nullptr; }
const char *fake_DirModel_entry_name(const DirModel *m, const FileEntry *e) { (void)m; (void)e; return g_fe_name; }
size_t fake_DirModel_count_selected(DirModel *m, int64_t *b) { (void)m; (void)b; return g_sel_cnt; }

int fake_arc_extract_selected(FilePane *fp, char *out)
{
    (void)fp;
    sc_put("call arc_extract_selected rc="); sc_num(g_arc_rc); sc_nl();
    if (g_arc_rc == 0) (void)str_copy(out, "/tmp/arc_item", PATH_MAX);
    return g_arc_rc;
}

int fake_pkg_vfs_read_uri(char **envp, const char *uri, char *out, int cap)
{
    sc_put("call pkg_vfs_read_uri env="); sc_num(envp != nullptr);
    sc_put(" u=|"); sc_put(uri); sc_put("| rc="); sc_num(g_vfs_rc); sc_nl();
    if (g_vfs_rc == 0) (void)str_copy(out, "/tmp/vfs_item", (size_t)cap);
    return g_vfs_rc;
}

int fake_sys_exec_sh_arg(const char *script, const char *arg1, char **envp)
{
    char cwd[PATH_MAX];
    if (os_getcwd(cwd, sizeof cwd) < 0) cwd[0] = '\0';
    sc_put("call sys_exec_sh_arg s=|"); sc_put(script ? script : "-");
    sc_put("| a1=|"); sc_put(arg1 ? arg1 : "-");
    sc_put("| env="); sc_num(envp != nullptr);
    sc_put(" in_act="); sc_num(str_cmp(cwd, g_lm.path) == 0); sc_nl();
    return 0;
}

void App_cmd_unpack(App *app) { (void)app; sc_put("call App_cmd_unpack\n"); }

#define FilePane_get_selected_entry fake_FilePane_get_selected_entry
#define DirModel_entry_name fake_DirModel_entry_name
#define DirModel_count_selected fake_DirModel_count_selected
#define arc_extract_selected fake_arc_extract_selected
#define pkg_vfs_read_uri fake_pkg_vfs_read_uri
#define sys_exec_sh_arg fake_sys_exec_sh_arg
#include CMDS_SRC
#undef FilePane_get_selected_entry
#undef DirModel_entry_name
#undef DirModel_count_selected
#undef arc_extract_selected
#undef pkg_vfs_read_uri
#undef sys_exec_sh_arg

#ifdef __x86_64__
void ref_App_cmd_newfile(App *app);
void ref_App_cmd_start_filter(App *app);
void ref_App_cmd_goto(App *app);
void ref_App_cmd_rename(App *app);
void ref_App_cmd_view(App *app);
void ref_App_cmd_edit(App *app);
void ref_App_cmd_shell(App *app);
void ref_App_cmd_copy(App *app);
void ref_App_cmd_move(App *app);
void ref_App_cmd_delete(App *app);
#endif

static int g_use_ref;
#ifdef __x86_64__
#define CALL(fn, ...) (g_use_ref ? ref_##fn(__VA_ARGS__) : fn(__VA_ARGS__))
#else
#define CALL(fn, ...) fn(__VA_ARGS__)
#endif

static int call_abi_safe(void (*fn)(App *), App *a)
{
#ifdef __x86_64__
    uint64_t swapped = 0;
    __asm__ volatile(
        "push %%rbx\n\t"
        "push %%r12\n\t"
        "movabs $0x1111222233334444, %%rbx\n\t"
        "movabs $0x5555666677778888, %%r12\n\t"
        "call *%%rsi\n\t"
        "movabs $0x1111222233334444, %%rax\n\t"
        "cmp %%rax, %%rbx\n\t"
        "setne %%dl\n\t"
        "movabs $0x5555666677778888, %%rax\n\t"
        "cmp %%rax, %%r12\n\t"
        "setne %%cl\n\t"
        "or %%cl, %%dl\n\t"
        "movzx %%dl, %%rax\n\t"
        "pop %%r12\n\t"
        "pop %%rbx\n\t"
        : "=a"(swapped)
        : "S"(fn), "D"(a)
        : "rcx", "rdx", "r8", "r9", "r10", "r11", "memory", "cc");
    return (int)swapped;
#else
    fn(a);
    return 0;
#endif
}

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
    (void)str_copy(g_lm.path, "/act/dir", sizeof g_lm.path);
    (void)str_copy(g_rm.path, "/other/dir", sizeof g_rm.path);
    (void)str_copy(g_lm.filter, "*.c", sizeof g_lm.filter);
    g_has_fe = 1;
    g_sel_cnt = 0;
    (void)str_copy(g_fe_name, "hello.txt", sizeof g_fe_name);
    g_arc_rc = 0;
    g_vfs_rc = 0;
}

static void dump_st(const char *tag)
{
    sc_put(tag); sc_put(" d="); sc_num((int64_t)g_app.active_dialog);
    sc_put(" rd="); sc_num(g_app.need_redraw);
    sc_put(" ff="); sc_num(g_scr.force_full); sc_nl();
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
        if (tag == 'N') { CALL(App_cmd_newfile, &g_app); dump_st("NF"); }
        else if (tag == 'F') { CALL(App_cmd_start_filter, &g_app); dump_st("FL"); }
        else if (tag == 'G') { CALL(App_cmd_goto, &g_app); dump_st("GT"); }
        else if (tag == 'R') {
            g_has_fe = (int)sc_tok_i64(&p);
            g_fe.flags = (uint32_t)sc_tok_i64(&p);
            CALL(App_cmd_rename, &g_app);
            dump_st("RN");
        } else if (tag == 'V') {
            g_has_fe = (int)sc_tok_i64(&p);
            g_fe.flags = (uint32_t)sc_tok_i64(&p);
            g_lm.virtual_mode = (uint32_t)sc_tok_i64(&p);
            g_arc_rc = (int)sc_tok_i64(&p);
            g_vfs_rc = (int)sc_tok_i64(&p);
            if (g_lm.virtual_mode == 3) (void)str_copy(g_lm.path, "vfs://box", sizeof g_lm.path);
            CALL(App_cmd_view, &g_app);
            dump_st("VW");
        } else if (tag == 'E') {
            g_has_fe = (int)sc_tok_i64(&p);
            g_fe.flags = (uint32_t)sc_tok_i64(&p);
            CALL(App_cmd_edit, &g_app);
            dump_st("ED");
        } else if (tag == 'S') {
            char dir[256]; sc_tok_str(&p, dir, sizeof dir);
            (void)str_copy(g_lm.path, dir, sizeof g_lm.path);
            char before[PATH_MAX], after[PATH_MAX];
            if (os_getcwd(before, sizeof before) < 0) before[0] = '\0';
            CALL(App_cmd_shell, &g_app);
            if (os_getcwd(after, sizeof after) < 0) after[0] = '\0';
            sc_put("SH cwd="); sc_num(str_cmp(before, after) == 0); sc_put(" ");
            dump_st("ST");
        } else if (tag == 'C' || tag == 'M' || tag == 'B') {
            int is_mv = (tag == 'M') || (tag == 'B' && sc_tok_i64(&p) != 0);
            if (tag != 'B') {
                g_sel_cnt = (size_t)sc_tok_i64(&p);
                g_has_fe = (int)sc_tok_i64(&p);
                g_fe.flags = (uint32_t)sc_tok_i64(&p);
            }
#ifdef __x86_64__
            void (*fn)(App *) = is_mv ? (g_use_ref ? ref_App_cmd_move : App_cmd_move)
                                      : (g_use_ref ? ref_App_cmd_copy : App_cmd_copy);
#else
            void (*fn)(App *) = is_mv ? App_cmd_move : App_cmd_copy;
#endif
            int sw = call_abi_safe(fn, &g_app);
            if (tag == 'B') { sc_put("ABI_CLOBBER="); sc_num(sw); sc_nl(); }
            else dump_st(is_mv ? "MV" : "CP");
        } else if (tag == 'A') {                                    /* an archive pane (C only): F5 unpacks, F6 / F8 are refused */
            int op = (int)sc_tok_i64(&p);
            g_lm.virtual_mode = 2;
            g_has_fe = 1;
            g_fe.flags = 0;
            g_sel_cnt = 0;
            if (op == 0) App_cmd_copy(&g_app);
            else if (op == 1) App_cmd_move(&g_app);
            else App_cmd_delete(&g_app);
            g_lm.virtual_mode = 0;
            dump_st("AR");
        } else if (tag == 'D') {
            g_sel_cnt = (size_t)sc_tok_i64(&p);
            g_has_fe = (int)sc_tok_i64(&p);
            g_fe.flags = (uint32_t)sc_tok_i64(&p);
            CALL(App_cmd_delete, &g_app);
            dump_st("DL");
        }
    }
    sc_flush();
    return 0;
}
