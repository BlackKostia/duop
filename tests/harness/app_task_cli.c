/* tests/harness/app_task_cli.c - A1.3 differential harness for src/app/app_task.c. */
#include "duop_app.h"
#include "duop_app_int.h"
#include "duop_classes.h"
#include "duop_fs.h"
#include "duop_oop.h"
#include "duop_pal.h"
#include "duop_string.h"
#include "duop_task.h"
#include "duop_tui.h"
#include "duop_ui.h"
#include "script_cli.h"

uint64_t g_task_skipped;                         /* TaskShared.skipped of the worker reaped last: read by the archive messages */
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

/* ---- fake state ---- */
static App g_app;
static FilePane g_pane;
static DirModel g_model;
static FileEntry g_fe[4];
static const char *g_names[4] = { "..", "one.txt", "two.txt", "three.txt" };
static int g_cur_idx;

static int g_fake_active;
static int g_fake_oom;
static intptr_t g_fake_stat_rc;
static uint32_t g_fake_stat_mode;
static intptr_t g_fake_start_rc;
static int g_fake_wake_fd = 42;
static int64_t g_now_ms = 1000;

static int g_fake_poll_done;
static int64_t g_fake_poll_res;
static int g_fake_cfl;
static int g_fake_paused;

const LangStrings *i18n_get(void) { return &lang_en_table; }

void *fake_mem_alloc(size_t n)
{
    if (g_fake_oom) return nullptr;
    return mem_alloc(n);
}

intptr_t fake_os_statat(int dirfd, const char *path, FileStat *st, int follow)
{
    sc_put("call os_statat dfd="); sc_num(dirfd); sc_put(" p=|"); sc_put(path ? path : "");
    sc_put("| fol="); sc_num(follow); sc_nl();
    if (g_fake_stat_rc < 0) return g_fake_stat_rc;
    mem_zero(st, sizeof *st);
    st->mode = g_fake_stat_mode;
    return 0;
}

int64_t fake_os_clock_ms(void) { return g_now_ms; }

int fake_fs_task_is_active(void) { return g_fake_active; }

intptr_t fake_fs_task_start(const TaskJob *job)
{
    sc_put("call fs_task_start op="); sc_num(job->op); sc_put(" cnt="); sc_num(job->count);
    sc_put(" join="); sc_num(job->join_dst); sc_put(" src=|"); sc_put(job->src_dir);
    sc_put("| dst=|"); sc_put(job->dst); sc_put("| nlen="); sc_num((int64_t)job->names_len);
    sc_put(" names=|");
    for (uint64_t i = 0; i < job->names_len; i++) {
        char c = job->names[i];
        if (c == '\0') sc_put(";");
        else { char s[2] = { c, 0 }; sc_put(s); }
    }
    sc_put("|\n");
    return g_fake_start_rc;
}

int fake_fs_task_wake_fd(void) { return g_fake_wake_fd; }

int fake_fs_task_poll(int64_t *out_res)
{
    if (!g_fake_poll_done) return 0;
    *out_res = g_fake_poll_res;
    return 1;
}

void fake_fs_task_snapshot(TaskSnap *out)
{
    out->done_files++;
    sc_put("call fs_task_snapshot df="); sc_num((int64_t)out->done_files); sc_nl();
}

int fake_fs_task_check_conflict(char *out_name, uint64_t *src_sz, uint64_t *dst_sz)
{
    if (!g_fake_cfl) return 0;
    (void)str_copy(out_name, "clash.bin", TASK_NAME_MAX);
    *src_sz = 1234;
    *dst_sz = 5678;
    return 1;
}

int fake_fs_task_toggle_pause(void)
{
    g_fake_paused ^= 1;
    sc_put("call fs_task_toggle_pause -> "); sc_num(g_fake_paused); sc_nl();
    return g_fake_paused;
}

void fake_fs_task_cancel(void) { sc_put("call fs_task_cancel\n"); }
void fake_fs_task_shutdown(void) { sc_put("call fs_task_shutdown\n"); }

void fake_fs_task_reply_conflict(int action, int for_all)
{
    sc_put("call fs_task_reply_conflict act="); sc_num(action); sc_put(" all="); sc_num(for_all); sc_nl();
}

void fake_input_set_wake_fd(int fd) { sc_put("call input_set_wake_fd "); sc_num(fd); sc_nl(); }

size_t fake_DirModel_count_selected(DirModel *m, int64_t *out_bytes)
{
    (void)out_bytes;
    size_t n = 0;
    for (uint64_t i = 0; i < m->count; i++)
        if (g_fe[i].flags & FE_SELECTED) n++;
    return n;
}

FileEntry *fake_DirModel_get_entry(DirModel *m, intptr_t idx)
{
    if (idx < 0 || (uint64_t)idx >= m->count) return nullptr;
    return &g_fe[idx];
}

const char *fake_DirModel_entry_name(const DirModel *m, const FileEntry *e)
{
    (void)m;
    intptr_t idx = e - g_fe;
    return (idx >= 0 && idx < 4) ? g_names[idx] : "";
}

FileEntry *fake_FilePane_get_selected_entry(FilePane *fp)
{
    (void)fp;
    if (g_cur_idx < 0 || (uint64_t)g_cur_idx >= g_model.count) return nullptr;
    return &g_fe[g_cur_idx];
}

const char *fs_errno_message(int neg_errno)
{
    if (neg_errno == -EBUSY) return "E_BUSY";
    if (neg_errno == -ENOMEM) return "E_NOMEM";
    if (neg_errno == -ECANCELED) return "E_CANCELED";
    if (neg_errno == -EACCES) return "E_ACCES";
    return "E_OTHER";
}

void App_set_status(App *app, const char *msg, int kind)
{
    sc_put("call App_set_status "); sc_num(app != nullptr); sc_put(" k="); sc_num(kind);
    sc_put(" m=|"); sc_put(msg ? msg : "(null)"); sc_put("|\n");
}

void App_reload_panes(App *app, const char *focus)
{
    sc_put("call App_reload_panes "); sc_num(app != nullptr); sc_put(" k="); sc_num(focus != nullptr); sc_nl();
}

void App_close_dialog(App *app, int result)
{
    sc_put("call App_close_dialog "); sc_num(app != nullptr); sc_put(" r="); sc_num(result); sc_nl();
}

static void fake_dlg_dtor(Object *self)
{
    Dialog *d = (Dialog *)self;
    sc_put("call dlg_dtor tag="); sc_num(d->op_tag); sc_nl();
}
static const ObjectVtbl g_fake_dlg_vtbl = { .super = nullptr, .class_name = "FakeDlg", .inst_size = sizeof(Dialog), .dtor = fake_dlg_dtor };

static Dialog *make_dlg(uint32_t tag)
{
    Dialog *d = Object_alloc(&g_fake_dlg_vtbl, sizeof(Dialog));
    if (d) d->op_tag = tag;
    return d;
}

ProgressDialog *ProgressDialog_new(App *app)
{
    sc_put("call ProgressDialog_new "); sc_num(app != nullptr); sc_nl();
    return (ProgressDialog *)make_dlg(OP_PROGRESS);
}

ConflictDialog *ConflictDialog_new(App *app, const char *name, uint64_t src_sz, uint64_t dst_sz)
{
    sc_put("call ConflictDialog_new "); sc_num(app != nullptr); sc_put(" n=|"); sc_put(name ? name : "");
    sc_put("| s="); sc_num((int64_t)src_sz); sc_put(" d="); sc_num((int64_t)dst_sz); sc_nl();
    return (ConflictDialog *)make_dlg(OP_CONFLICT);
}

void ProgressDialog_format_summary(const void *snap, char *buf)
{
    const TaskSnap *ts = (const TaskSnap *)snap;
    sc_put("call ProgressDialog_format_summary op="); sc_num(ts->op); sc_nl();
    (void)str_copy(buf, "SUMMARY_TEXT", 64);
}

ConfirmDialog *ConfirmDialog_new(App *app, const char *title, const char *msg, const char *detail, uint32_t tag)
{
    sc_put("call ConfirmDialog_new "); sc_num(app != nullptr); sc_put(" t=|"); sc_put(title ? title : "");
    sc_put("| m=|"); sc_put(msg ? msg : ""); sc_put("| d=|"); sc_put(detail ? detail : "");
    sc_put("| tag="); sc_num(tag); sc_nl();
    return (ConfirmDialog *)make_dlg(tag);
}

/* ---- include C module under test ---- */
#define mem_alloc fake_mem_alloc
#define os_statat fake_os_statat
#define os_clock_ms fake_os_clock_ms
#define fs_task_is_active fake_fs_task_is_active
#define fs_task_start fake_fs_task_start
#define fs_task_wake_fd fake_fs_task_wake_fd
#define fs_task_poll fake_fs_task_poll
#define fs_task_snapshot fake_fs_task_snapshot
#define fs_task_check_conflict fake_fs_task_check_conflict
#define fs_task_toggle_pause fake_fs_task_toggle_pause
#define fs_task_cancel fake_fs_task_cancel
#define fs_task_shutdown fake_fs_task_shutdown
#define fs_task_reply_conflict fake_fs_task_reply_conflict
#define input_set_wake_fd fake_input_set_wake_fd
#define DirModel_count_selected fake_DirModel_count_selected
#define DirModel_get_entry fake_DirModel_get_entry
#define DirModel_entry_name fake_DirModel_entry_name
#define FilePane_get_selected_entry fake_FilePane_get_selected_entry
#include TASK_SRC
#undef mem_alloc
#undef os_statat
#undef os_clock_ms
#undef fs_task_is_active
#undef fs_task_start
#undef fs_task_wake_fd
#undef fs_task_poll
#undef fs_task_snapshot
#undef fs_task_check_conflict
#undef fs_task_toggle_pause
#undef fs_task_cancel
#undef fs_task_shutdown
#undef fs_task_reply_conflict
#undef input_set_wake_fd
#undef DirModel_count_selected
#undef DirModel_get_entry
#undef DirModel_entry_name
#undef FilePane_get_selected_entry

#ifdef __x86_64__
void ref_task_status_errno(App *app, int64_t neg_errno);
void ref_App_task_start(App *app, int op, const char *dst_input);
void ref_App_task_tick(App *app);
void ref_App_task_show(App *app);
void ref_App_task_hide(App *app);
void ref_App_task_toggle_pause(App *app);
void ref_App_task_cancel(App *app);
void ref_App_task_confirm_quit(App *app);
void ref_App_task_shutdown(App *app);
void ref_App_task_conflict_done(App *app, int result, ConflictDialog *dlg);
#endif

static int g_use_ref;
#ifdef __x86_64__
#define CALL(fn, ...) (g_use_ref ? ref_##fn(__VA_ARGS__) : fn(__VA_ARGS__))
#else
#define CALL(fn, ...) fn(__VA_ARGS__)
#endif

static void reset_app(void)
{
    if (g_app.active_dialog) {
        Object_free((Object *)(uintptr_t)g_app.active_dialog);
        g_app.active_dialog = 0;
    }
    mem_zero(&g_app, sizeof g_app);
    mem_zero(&g_pane, sizeof g_pane);
    mem_zero(&g_model, sizeof g_model);
    mem_zero(g_fe, sizeof g_fe);
    g_app.active_pane = (uint64_t)(uintptr_t)&g_pane;
    g_pane.model = (uint64_t)(uintptr_t)&g_model;
    (void)str_copy(g_model.path, "/src/dir", sizeof g_model.path);
    g_model.count = 4;
    g_fe[0].flags = FE_IS_PARENT;
    g_cur_idx = 1;
    g_fake_active = 0;
    g_fake_oom = 0;
    g_fake_stat_rc = 0;
    g_fake_stat_mode = S_IFDIR | 0755;
    g_fake_start_rc = 0;
    g_fake_poll_done = 0;
    g_fake_poll_res = 0;
    g_fake_cfl = 0;
    g_fake_paused = 0;
    g_now_ms = 1000;
}

static void dump_state(const char *tag)
{
    TaskSnap *ts = (TaskSnap *)(void *)g_app.task_snap;
    Dialog *ad = (Dialog *)(uintptr_t)g_app.active_dialog;
    sc_put(tag); sc_put(" act="); sc_num(g_app.task_active);
    sc_put(" hid="); sc_num(g_app.task_hidden);
    sc_put(" rd="); sc_num(g_app.need_redraw);
    sc_put(" dlg="); sc_num(ad ? (int64_t)ad->op_tag : 0);
    sc_put(" at="); sc_num((int64_t)g_app.task_dlg_at);
    sc_put(" last="); sc_num((int64_t)g_app.task_last_ms);
    sc_put(" op="); sc_num(ts->op); sc_put(" ph="); sc_num(ts->phase);
    sc_put(" p="); sc_num(ts->paused); sc_put(" c="); sc_num(ts->canceling);
    sc_put(" eta="); sc_num((int64_t)ts->eta_sec); sc_nl();
}

static void handle_start(char *p)
{
    /* S <active> <op> <sel_mask> <cur_idx> <oom> <stat_rc> <is_dir> <start_rc> <dst: -=NULL, !=empty, else str> */
    reset_app();
    g_fake_active = (int)sc_tok_i64(&p);
    int op = (int)sc_tok_i64(&p);
    int mask = (int)sc_tok_i64(&p);
    g_cur_idx = (int)sc_tok_i64(&p);
    g_fake_oom = (int)sc_tok_i64(&p);
    g_fake_stat_rc = sc_tok_i64(&p);
    g_fake_stat_mode = sc_tok_i64(&p) ? (S_IFDIR | 0755) : (S_IFREG | 0644);
    g_fake_start_rc = sc_tok_i64(&p);
    char dstb[64];
    sc_tok_str(&p, dstb, sizeof dstb);
    for (int i = 0; i < 4; i++) if (mask & (1 << i)) g_fe[i].flags |= FE_SELECTED;
    const char *dst = (dstb[0] == '-' && dstb[1] == '\0') ? nullptr
                    : (dstb[0] == '!' && dstb[1] == '\0') ? "" : dstb;
    CALL(App_task_start, &g_app, op, dst);
    dump_state("ST");
}

static void handle_tick(char *p)
{
    /* T <active> <poll_done> <poll_res> <now_ms> <last_ms> <dlg_at> <hidden> <cfl> <cur_dlg_tag> <snap_op> */
    reset_app();
    g_app.task_active = (uint32_t)sc_tok_i64(&p);
    g_fake_poll_done = (int)sc_tok_i64(&p);
    g_fake_poll_res = sc_tok_i64(&p);
    g_now_ms = sc_tok_i64(&p);
    g_app.task_last_ms = (uint64_t)sc_tok_i64(&p);
    g_app.task_dlg_at = (uint64_t)sc_tok_i64(&p);
    g_app.task_hidden = (uint32_t)sc_tok_i64(&p);
    g_fake_cfl = (int)sc_tok_i64(&p);
    uint32_t cur_tag = (uint32_t)sc_tok_i64(&p);
    uint32_t sop = (uint32_t)sc_tok_i64(&p);
    ((TaskSnap *)(void *)g_app.task_snap)->op = sop;
    if (cur_tag) g_app.active_dialog = (uint64_t)(uintptr_t)make_dlg(cur_tag);
    CALL(App_task_tick, &g_app);
    dump_state("TK");
}

static void handle_ctl(char *p)
{
    int sub = (int)sc_tok_i64(&p);
    if (sub == 0) {
        /* C 0 <neg_errno> */
        CALL(task_status_errno, &g_app, sc_tok_i64(&p));
    } else if (sub == 1) {
        /* C 1 <active> <has_dlg> : App_task_show */
        reset_app();
        g_app.task_active = (uint32_t)sc_tok_i64(&p);
        g_app.task_hidden = 1;
        if (sc_tok_i64(&p)) g_app.active_dialog = (uint64_t)(uintptr_t)make_dlg(OP_QUIT);
        CALL(App_task_show, &g_app);
        dump_state("SH");
    } else if (sub == 2) {
        /* C 2 : App_task_hide */
        reset_app();
        CALL(App_task_hide, &g_app);
        dump_state("HI");
    } else if (sub == 3) {
        /* C 3 : App_task_toggle_pause + App_task_cancel */
        reset_app();
        CALL(App_task_toggle_pause, &g_app);
        dump_state("PA");
        CALL(App_task_cancel, &g_app);
        dump_state("CA");
    } else if (sub == 4) {
        /* C 4 <has_dlg> : App_task_confirm_quit */
        reset_app();
        ((TaskSnap *)(void *)g_app.task_snap)->op = TASK_OP_MOVE;
        if (sc_tok_i64(&p)) g_app.active_dialog = (uint64_t)(uintptr_t)make_dlg(OP_PROGRESS);
        CALL(App_task_confirm_quit, &g_app);
        dump_state("CQ");
    } else if (sub == 5) {
        /* C 5 : App_task_shutdown */
        reset_app();
        g_app.task_active = 1;
        CALL(App_task_shutdown, &g_app);
        dump_state("SD");
    } else if (sub == 6) {
        /* C 6 <res> <act> <all> : App_task_conflict_done */
        reset_app();
        int res = (int)sc_tok_i64(&p);
        ConflictDialog cd; mem_zero(&cd, sizeof cd);
        cd.action = (uint32_t)sc_tok_i64(&p);
        cd.for_all = (uint32_t)sc_tok_i64(&p);
        CALL(App_task_conflict_done, &g_app, res, &cd);
        dump_state("CD");
    }
}

int duop_main(int argc, char **argv, char **envp, const uintptr_t *auxv)
{
    (void)envp;
    (void)auxv;
    g_use_ref = (argc >= 2 && argv[1][0] == 'r');
    char *script = sc_read_all();
    for (char *line; (line = sc_next_line(&script));) {
        char *p = line;
        char tag = *p++;
        if (tag == 'S') handle_start(p);
        else if (tag == 'T') handle_tick(p);
        else if (tag == 'C') handle_ctl(p);
    }
    reset_app();
    sc_flush();
    return 0;
}
