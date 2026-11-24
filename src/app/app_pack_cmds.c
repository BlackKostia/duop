/* src/app/app_pack_cmds.c - the pack command of the archive layer (freestanding C23).
 *
 * Alt+F5 opens the Pack dialog on the marked entries of the active pane (the entry under the cursor when none is marked), prefilled with the
 * other pane's folder and a name made from the selection (the entry's name, or the folder's when several are marked) plus ".tar.gz". The
 * extension of the name picked in the dialog chooses the format (.tar, .tar.gz / .tgz, .zip); confirming starts the TaskWorker with
 * TASK_OP_PACK (base folder = job->src_dir, entries = job->names, archive = job->dst, format = job->join_dst). An archive that exists is
 * never replaced. The engine is src/fs/arc_pack.c; the result message comes from app_task.c. */
#include "duop_app.h"
#include "duop_app_int.h"
#include "duop_arcpack.h"
#include "duop_arcunpack.h"
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

/* the entries to pack: the marked ones, else the one under the cursor; "name\0name\0..." into out (cap bytes), the count, and the bytes used */
static size_t selected_names(DirModel *m, FilePane *pane, char *out, size_t cap, uint32_t *count)
{
    size_t used = 0;
    uint32_t n = 0;
    bool marked = DirModel_count_selected(m, nullptr) > 0;
    FileEntry *cur = FilePane_get_selected_entry(pane);
    for (uint64_t i = 0; i < m->count; i++) {
        FileEntry *fe = DirModel_get_entry(m, (intptr_t)i);
        if (!fe || (fe->flags & FE_IS_PARENT)) continue;
        if (!(marked ? (fe->flags & FE_SELECTED) : fe == cur)) continue;
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

static size_t names_size(DirModel *m)
{
    size_t total = 0;
    for (uint64_t i = 0; i < m->count; i++) {
        FileEntry *fe = DirModel_get_entry(m, (intptr_t)i);
        if (fe && !(fe->flags & FE_IS_PARENT)) total += str_len(DirModel_entry_name(m, fe)) + 1;
    }
    return total;
}

void App_cmd_pack(App *app)
{
    FilePane *pane = (FilePane *)(uintptr_t)app->active_pane;
    DirModel *m = (DirModel *)(uintptr_t)pane->model;
    const LangStrings *lang = i18n_get();
    if (m->virtual_mode == 2) {
        App_set_status(app, lang->s[LS_arc_readonly], MSG_WARN);
        return;
    }
    FileEntry *cur = FilePane_get_selected_entry(pane);
    size_t marked = DirModel_count_selected(m, nullptr);
    if (m->virtual_mode != 0 || (!marked && (!cur || (cur->flags & FE_IS_PARENT)))) {
        App_set_status(app, lang->s[LS_pack_nothing], MSG_WARN);
        return;
    }
    char name[300];
    const char *stem = marked == 1 || !marked ? nullptr : path_basename(m->path);
    if (!stem) {
        char *one = mem_alloc(names_size(m) + 1);
        uint32_t n = 0;
        if (!one) return;
        (void)selected_names(m, pane, one, names_size(m), &n);
        (void)str_copy(name, one, 256);
        mem_free(one);
    } else {
        (void)str_copy(name, stem[0] ? stem : "archive", 256);
    }
    (void)str_cat(name, ".tar.gz", sizeof name);
    DirModel *om = (DirModel *)(uintptr_t)get_inactive_pane(app)->model;
    char full[PATH_MAX];
    if (path_join(full, om->virtual_mode == 0 ? om->path : m->path, name, sizeof full) < 0) {
        task_status_errno(app, -ENAMETOOLONG);
        return;
    }
    app->active_dialog = (uint64_t)(uintptr_t)InputDialog_new(app, lang->s[LS_app_title_pack], lang->s[LS_app_prm_pack], full, OP_PACK);
    app->need_redraw = 1;
}

void App_pack_confirm(App *app, const char *text)
{
    if (!text || !text[0]) return;
    FilePane *pane = (FilePane *)(uintptr_t)app->active_pane;
    DirModel *m = (DirModel *)(uintptr_t)pane->model;
    const LangStrings *lang = i18n_get();
    if (fs_task_is_active()) {
        task_status_errno(app, -EBUSY);
        return;
    }
    char dst[PATH_MAX];
    if (text[0] == '/') (void)str_copy(dst, text, sizeof dst);
    else if (path_join(dst, m->path, text, sizeof dst) < 0) {
        task_status_errno(app, -ENAMETOOLONG);
        return;
    }
    int fmt = arc_pack_format(dst);
    if (fmt < 0) {
        App_set_status(app, lang->s[LS_pack_format], MSG_WARN);
        return;
    }
    FileStat st;
    if (os_statat(AT_FDCWD, dst, &st, 0) == 0) {
        App_set_status(app, lang->s[LS_pack_exists], MSG_WARN);
        return;
    }
    size_t cap = names_size(m);
    TaskJob *job = mem_alloc(sizeof *job + cap + 16);
    if (!job) {
        task_status_errno(app, -ENOMEM);
        return;
    }
    mem_zero(job, sizeof *job);
    (void)str_copy(job->src_dir, m->path, PATH_MAX);
    (void)str_copy(job->dst, dst, PATH_MAX);
    job->names_len = selected_names(m, pane, job->names, cap, &job->count);
    if (!job->count) {
        App_set_status(app, lang->s[LS_pack_nothing], MSG_WARN);
        mem_free(job);
        return;
    }
    job->op = TASK_OP_PACK;
    job->join_dst = (uint32_t)fmt;
    g_task_run_hook = arc_task_run;
    App_task_launch(app, job);
    mem_free(job);
}
