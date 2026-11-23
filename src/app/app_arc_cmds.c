/* src/app/app_arc_cmds.c - the unpack and test commands of the archive layer (freestanding C23).
 *
 * Alt+F9 (and F5 inside an archive pane) opens the Unpack dialog, prefilled with the other pane's folder; confirming starts the
 * TaskWorker with TASK_OP_UNPACK (archive = job->src_dir, destination = job->dst, members = "<folder>\0<member>\0..."). Inside an
 * archive the members are the marked entries, else the one under the cursor, else - on ".." - everything in the folder shown; on an
 * archive file of a normal pane the whole archive is unpacked with "smart" unpacking (a subfolder when it has several top-level
 * entries). Alt+Shift+F9 starts TASK_OP_ARCTEST: every member decoded and verified, nothing written. */
#include "duop_app.h"
#include "duop_app_int.h"
#include "duop_arcunpack.h"
#include "duop_arcvfs.h"
#include "duop_classes.h"
#include "duop_fs.h"
#include "duop_oop.h"
#include "duop_pal.h"
#include "duop_string.h"
#include "duop_task.h"
#include "duop_tui.h"
#include "duop_ui.h"
#include "i18n_slots.h"

void task_status_errno(App *app, int64_t neg_errno);

/* the archive file the active pane stands for into out: 0, or the status text for "this is not an archive" */
static const char *archive_of(DirModel *m, FilePane *pane, char *out)
{
    const LangStrings *lang = i18n_get();
    if (m->virtual_mode == 2) {
        (void)str_copy(out, m->path, PATH_MAX);
        return nullptr;
    }
    FileEntry *fe = FilePane_get_selected_entry(pane);
    if (m->virtual_mode != 0 || !fe || (fe->flags & (FE_IS_DIR | FE_IS_PARENT))) return lang->s[LS_arc_not_archive];
    const char *name = DirModel_entry_name(m, fe);
    if (!arc_is_archive(name)) return lang->s[LS_arc_not_archive];
    return path_join(out, m->path, name, PATH_MAX) < 0 ? lang->s[LS_arc_not_archive] : nullptr;
}

void App_cmd_unpack(App *app)
{
    FilePane *pane = (FilePane *)(uintptr_t)app->active_pane;
    DirModel *m = (DirModel *)(uintptr_t)pane->model;
    if (fs_task_is_active()) {
        task_status_errno(app, -EBUSY);
        return;
    }
    char *ar = mem_alloc(PATH_MAX);
    if (!ar) return;
    const char *err = archive_of(m, pane, ar);
    mem_free(ar);
    const LangStrings *lang = i18n_get();
    if (err) {
        App_set_status(app, err, MSG_WARN);
        return;
    }
    DirModel *im = (DirModel *)(uintptr_t)get_inactive_pane(app)->model;
    const char *init = im->virtual_mode == 0 ? im->path : (m->virtual_mode == 2 ? arc_vfs_orig(m) : m->path);
    app->active_dialog = (uint64_t)(uintptr_t)InputDialog_new(app, lang->s[LS_app_title_unpack], lang->s[LS_app_prm_unpack], init, OP_UNPACK);
    app->need_redraw = 1;
}

/* the members to unpack from the folder an archive pane shows: marked entries, else the cursor's, on ".." everything: names into job, bytes used */
static size_t member_names(DirModel *m, FilePane *pane, char *out, size_t cap, uint32_t *count)
{
    size_t used = 0;
    uint32_t n = 0;
    size_t marked = DirModel_count_selected(m, nullptr);
    FileEntry *cur = FilePane_get_selected_entry(pane);
    bool all = !marked && cur && (cur->flags & FE_IS_PARENT);
    for (uint64_t i = 0; i < m->count; i++) {
        FileEntry *fe = DirModel_get_entry(m, (intptr_t)i);
        if (!fe || (fe->flags & FE_IS_PARENT)) continue;
        if (!(all || (marked ? (fe->flags & FE_SELECTED) : fe == cur))) continue;
        const char *name = DirModel_entry_name(m, fe);
        size_t l = str_len(name) + 1;
        if (used + l > cap) break;
        __builtin_memcpy(out + used, name, l);
        used += l;
        n++;
    }
    *count = n;
    return used;
}

static size_t members_size(DirModel *m)
{
    size_t total = 0;
    for (uint64_t i = 0; i < m->count; i++) {
        FileEntry *fe = DirModel_get_entry(m, (intptr_t)i);
        if (fe) total += str_len(DirModel_entry_name(m, fe)) + 1;
    }
    return total;
}

/* starts a task on the archive of the active pane; sub / members / dst may be empty */
static void start_archive_task(App *app, uint32_t op, const char *dst)
{
    FilePane *pane = (FilePane *)(uintptr_t)app->active_pane;
    DirModel *m = (DirModel *)(uintptr_t)pane->model;
    if (fs_task_is_active()) {
        task_status_errno(app, -EBUSY);
        return;
    }
    bool inside = m->virtual_mode == 2;
    size_t cap = inside ? members_size(m) : 0;
    const char *sub = inside ? arc_vfs_sub(m) : "";
    TaskJob *job = mem_alloc(sizeof *job + str_len(sub) + 1 + cap + 16);
    if (!job) {
        task_status_errno(app, -ENOMEM);
        return;
    }
    mem_zero(job, sizeof *job);
    const char *err = archive_of(m, pane, job->src_dir);
    if (err) {
        App_set_status(app, err, MSG_WARN);
        mem_free(job);
        return;
    }
    job->op = op;
    size_t sl = str_copy(job->names, sub, str_len(sub) + 1);
    job->names[sl] = 0;
    size_t used = sl + 1;
    if (op == TASK_OP_UNPACK) {
        const char *base = inside ? arc_vfs_orig(m) : m->path;
        if (dst[0] == '/') (void)str_copy(job->dst, dst, PATH_MAX);
        else if (path_join(job->dst, base, dst, PATH_MAX) < 0) {
            task_status_errno(app, -ENAMETOOLONG);
            mem_free(job);
            return;
        }
        if (inside) used += member_names(m, pane, job->names + used, cap, &job->count);
        else job->join_dst = 2;                                                /* the whole archive, smart */
    }
    job->names_len = used;
    g_task_run_hook = arc_task_run;
    App_task_launch(app, job);
    mem_free(job);
}

void App_unpack_confirm(App *app, const char *dst)
{
    if (dst && dst[0]) start_archive_task(app, TASK_OP_UNPACK, dst);
}

void App_cmd_arctest(App *app)
{
    start_archive_task(app, TASK_OP_ARCTEST, "");
}
