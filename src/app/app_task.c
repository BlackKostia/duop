/* src/app/app_task.c - App Background Task Controller
 * (freestanding C23; replaces src/app/app_task.s and src/app/app_task_tick.s,
 * kept as tests/harness/app_task{,_tick}_reference.s). */
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
#include "i18n_slots.h"

#define TASK_DLG_DELAY_MS 300
#define TASK_TICK_MS 100

static inline TaskSnap *app_snap(App *app) { return (TaskSnap *)(void *)app->task_snap; }

void task_status_errno(App *app, int64_t neg_errno)
{
    const char *msg = fs_errno_message((int)neg_errno);
    int kind = (neg_errno == -ECANCELED) ? MSG_WARN : MSG_ERR;
    App_set_status(app, msg, kind);
}

static size_t sum_marked_names(DirModel *m)
{
    size_t total = 0;
    for (uint64_t i = 0; i < m->count; i++) {
        FileEntry *fe = DirModel_get_entry(m, (intptr_t)i);
        if (!fe || !(fe->flags & FE_SELECTED)) continue;
        total += str_len(DirModel_entry_name(m, fe)) + 1;
    }
    return total;
}

static void fill_job_names(App *app, DirModel *m, TaskJob *job, size_t sel_count, size_t names_len)
{
    if (sel_count == 0) {
        FilePane *pane = (FilePane *)(uintptr_t)app->active_pane;
        FileEntry *fe = FilePane_get_selected_entry(pane);
        if (fe) (void)str_copy(job->names, DirModel_entry_name(m, fe), names_len);
        return;
    }
    char *dst = job->names;
    for (uint64_t i = 0; i < m->count; i++) {
        FileEntry *fe = DirModel_get_entry(m, (intptr_t)i);
        if (!fe || !(fe->flags & FE_SELECTED)) continue;
        const char *name = DirModel_entry_name(m, fe);
        size_t sz = str_len(name) + 1;
        (void)str_copy(dst, name, sz);
        dst += sz;
    }
}

static int setup_job_dst(App *app, TaskJob *job, int op, const char *dst_input, size_t count, size_t sel_count)
{
    if (op == TASK_OP_DELETE) return 0;
    (void)str_copy(job->dst, dst_input, PATH_MAX);
    job->join_dst = 1;
    if (count == 1 && sel_count == 0) {
        FileStat st;
        intptr_t sr = os_statat(AT_FDCWD, dst_input, &st, 1);
        if (sr < 0) {
            if (sr != -ENOENT) {
                task_status_errno(app, sr);
                return -1;
            }
            job->join_dst = 0;
        } else {
            job->join_dst = ((st.mode & S_IFMT) == S_IFDIR) ? 1u : 0u;
        }
    }
    return 0;
}

void App_task_start(App *app, int op, const char *dst_input)
{
    if (fs_task_is_active()) {
        task_status_errno(app, -EBUSY);
        return;
    }
    if (op != TASK_OP_DELETE && (!dst_input || dst_input[0] == '\0')) return;

    FilePane *pane = (FilePane *)(uintptr_t)app->active_pane;
    DirModel *m = (DirModel *)(uintptr_t)pane->model;
    size_t sel_count = DirModel_count_selected(m, nullptr);
    size_t count = sel_count;
    size_t names_len = 0;

    if (sel_count == 0) {
        FileEntry *fe = FilePane_get_selected_entry(pane);
        if (!fe) return;
        if (fe->flags & FE_IS_PARENT) {
            App_set_status(app, i18n_get()->s[LS_app_err_parent], MSG_WARN);
            return;
        }
        names_len = str_len(DirModel_entry_name(m, fe)) + 1;
        count = 1;
    } else {
        names_len = sum_marked_names(m);
    }

    TaskJob *job = mem_alloc(sizeof(TaskJob) + names_len + 16);
    if (!job) {
        task_status_errno(app, -ENOMEM);
        return;
    }
    job->op = (uint32_t)op;
    job->count = (uint32_t)count;
    (void)str_copy(job->src_dir, m->path, PATH_MAX);
    if (setup_job_dst(app, job, op, dst_input, count, sel_count) < 0) {
        mem_free(job);
        return;
    }
    fill_job_names(app, m, job, sel_count, names_len);
    job->names_len = (uint64_t)names_len;
    App_task_launch(app, job);
    mem_free(job);
}

/* starts the worker for job (the caller frees it) and sets up the UI state of a running task */
void App_task_launch(App *app, const TaskJob *job)
{
    intptr_t rc = fs_task_start(job);
    if (rc < 0) {
        task_status_errno(app, rc);
        return;
    }
    int op = (int)job->op;

    TaskSnap *snap = app_snap(app);
    mem_zero(snap, sizeof *snap);
    snap->op = (uint32_t)op;
    snap->phase = TASK_PHASE_SCAN;
    snap->eta_sec = UINT64_MAX;
    app->task_active = 1;
    app->task_hidden = 0;
    app->task_last_ms = 0;
    app->task_dlg_at = (uint64_t)(os_clock_ms() + TASK_DLG_DELAY_MS);
    input_set_wake_fd(fs_task_wake_fd());
    app->need_redraw = 1;
}

/* the status line after an unpack or a test: what failed as a whole, or how many members were left out */
static void task_finish_archive(App *app, uint32_t op, int64_t result)
{
    const LangStrings *lang = i18n_get();
    if (op == TASK_OP_PACK && result == -EEXIST) {
        App_set_status(app, lang->s[LS_pack_exists], MSG_WARN);
        return;
    }
    if (op == TASK_OP_PACK && result != 0) {
        task_status_errno(app, result);
        return;
    }
    if (result == -EFBIG) {
        App_set_status(app, lang->s[LS_arc_bomb], MSG_ERR);
    } else if (result == -EINVAL || result == -EOPNOTSUPP) {
        App_set_status(app, lang->s[LS_arc_not_archive], MSG_ERR);
    } else if (result != 0) {
        task_status_errno(app, result);
    } else if (g_task_skipped) {
        char msg[512];
        size_t n = str_copy(msg, lang->s[LS_arc_issues_pfx], sizeof msg);
        n += u64_to_dec(g_task_skipped, msg + n);
        (void)str_copy(msg + n, lang->s[LS_arc_issues_sfx], sizeof msg - n);
        App_set_status(app, msg, MSG_WARN);
    } else {
        App_set_status(app, op == TASK_OP_ARCTEST ? lang->s[LS_app_ok_arctest] : op == TASK_OP_PACK ? lang->s[LS_app_ok_pack] : lang->s[LS_app_ok_unpack], MSG_OK);
    }
}

static void task_finalize(App *app, int64_t result)
{
    app->task_active = 0;
    input_set_wake_fd(-1);
    Dialog *ad = (Dialog *)(uintptr_t)app->active_dialog;
    if (ad && ad->op_tag == OP_PROGRESS) {
        app->active_dialog = 0;
        Object_free((Object *)ad);
    }
    App_reload_panes(app, nullptr);
    uint32_t op = app_snap(app)->op;
    if (op == TASK_OP_UNPACK || op == TASK_OP_ARCTEST || op == TASK_OP_PACK) {
        task_finish_archive(app, op, result);
    } else if (result != 0) {
        task_status_errno(app, result);
    } else {
        const LangStrings *lang = i18n_get();
        const char *msg = lang->s[LS_app_ok_copy];
        if (op == TASK_OP_DELETE) msg = lang->s[LS_app_ok_delete];
        else if (op == TASK_OP_MOVE) msg = lang->s[LS_app_ok_move];
        App_set_status(app, msg, MSG_OK);
    }
    app->need_redraw = 1;
}

void App_task_tick(App *app)
{
    if (!app->task_active) return;
    int64_t res = 0;
    if (fs_task_poll(&res)) {
        task_finalize(app, res);
        return;
    }
    uint64_t now = (uint64_t)os_clock_ms();
    if (now - app->task_last_ms >= (uint64_t)(TASK_TICK_MS - 20)) {
        app->task_last_ms = now;
        fs_task_snapshot(app_snap(app));
        app->need_redraw = 1;
    }
    char cname[TASK_NAME_MAX + 1];
    uint64_t src_sz = 0, dst_sz = 0;
    if (fs_task_check_conflict(cname, &src_sz, &dst_sz)) {
        Dialog *ad = (Dialog *)(uintptr_t)app->active_dialog;
        if (!ad || ad->op_tag == OP_PROGRESS) {
            if (ad) {
                app->active_dialog = 0;
                Object_free((Object *)ad);
            }
            app->active_dialog = (uint64_t)(uintptr_t)ConflictDialog_new(app, cname, src_sz, dst_sz);
            app->need_redraw = 1;
            return;
        }
    }
    if (app->active_dialog == 0 && app->task_hidden == 0 && now >= app->task_dlg_at)
        App_task_show(app);
}

void App_task_show(App *app)
{
    if (!app->task_active || app->active_dialog != 0) return;
    fs_task_snapshot(app_snap(app));
    app->active_dialog = (uint64_t)(uintptr_t)ProgressDialog_new(app);
    app->task_hidden = 0;
    app->need_redraw = 1;
}

void App_task_hide(App *app)
{
    app->task_hidden = 1;
    App_close_dialog(app, DLG_CANCEL);
}

void App_task_toggle_pause(App *app)
{
    app_snap(app)->paused = (uint32_t)fs_task_toggle_pause();
    app->need_redraw = 1;
}

void App_task_cancel(App *app)
{
    fs_task_cancel();
    TaskSnap *snap = app_snap(app);
    snap->canceling = 1;
    snap->paused = 0;
    app->need_redraw = 1;
}

void App_task_confirm_quit(App *app)
{
    if (app->active_dialog != 0) return;
    char detail[256];
    ProgressDialog_format_summary(app_snap(app), detail);
    const LangStrings *lang = i18n_get();
    app->active_dialog = (uint64_t)(uintptr_t)ConfirmDialog_new(
        app, lang->s[LS_app_title_quit_task], lang->s[LS_app_msg_quit_task], detail, OP_QUIT);
    app->need_redraw = 1;
}

void App_task_shutdown(App *app)
{
    fs_task_shutdown();
    app->task_active = 0;
    input_set_wake_fd(-1);
}

void App_task_conflict_done(App *app, int result, ConflictDialog *dlg)
{
    if (result == DLG_OK) {
        fs_task_reply_conflict((int)dlg->action, (int)dlg->for_all);
        return;
    }
    fs_task_reply_conflict(CONFLICT_ACT_SKIP, 1);
    App_task_cancel(app);
}
