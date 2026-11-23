/* src/app/app_cmds.c - App file/editor/shell/transfer/delete command dispatchers
 * (freestanding C23; replaces src/app/app_cmds.s, kept as tests/harness/app_cmds_reference.s). */
#include "duop_app.h"
#include "duop_app_int.h"
#include "duop_classes.h"
#include "duop_fs.h"
#include "duop_pal.h"
#include "duop_pkg.h"
#include "duop_string.h"
#include "duop_sys.h"
#include "duop_tui.h"
#include "duop_ui.h"
#include "i18n_slots.h"

static void open_input_dlg(App *app, const char *title, const char *prompt, const char *init, uint32_t op_tag)
{
    app->active_dialog = (uint64_t)(uintptr_t)InputDialog_new(app, title, prompt, init, op_tag);
    app->need_redraw = 1;
}

void App_cmd_newfile(App *app)
{
    const LangStrings *lang = i18n_get();
    open_input_dlg(app, lang->s[LS_app_title_newfile], lang->s[LS_app_prm_newfile], nullptr, OP_NEWFILE);
}

void App_cmd_start_filter(App *app)
{
    FilePane *pane = (FilePane *)(uintptr_t)app->active_pane;
    DirModel *m = (DirModel *)(uintptr_t)pane->model;
    const LangStrings *lang = i18n_get();
    open_input_dlg(app, lang->s[LS_app_title_filter], lang->s[LS_app_prm_filter], m->filter, OP_FILTER);
}

void App_cmd_goto(App *app)
{
    FilePane *pane = (FilePane *)(uintptr_t)app->active_pane;
    DirModel *m = (DirModel *)(uintptr_t)pane->model;
    const LangStrings *lang = i18n_get();
    open_input_dlg(app, lang->s[LS_app_title_goto], lang->s[LS_app_prm_goto], m->path, OP_GOTO);
}

void App_cmd_rename(App *app)
{
    FilePane *pane = (FilePane *)(uintptr_t)app->active_pane;
    FileEntry *fe = FilePane_get_selected_entry(pane);
    if (!fe) return;
    if (fe->flags & FE_IS_PARENT) {
        App_set_status(app, i18n_get()->s[LS_app_err_parent], MSG_WARN);
        return;
    }
    DirModel *m = (DirModel *)(uintptr_t)pane->model;
    const char *name = DirModel_entry_name(m, fe);
    const LangStrings *lang = i18n_get();
    open_input_dlg(app, lang->s[LS_app_title_rename], lang->s[LS_app_prm_rename], name, OP_RENAME);
}

void App_cmd_view(App *app)
{
    FilePane *pane = (FilePane *)(uintptr_t)app->active_pane;
    FileEntry *fe = FilePane_get_selected_entry(pane);
    if (!fe) return;
    if (fe->flags & (FE_IS_DIR | FE_LINK_DIR)) {
        App_set_status(app, i18n_get()->s[LS_app_err_isdir], MSG_WARN);
        return;
    }
    char full[PATH_MAX];
    DirModel *m = (DirModel *)(uintptr_t)pane->model;
    if (m->virtual_mode == 2) {
        int rc = arc_extract_selected(pane, full);
        if (rc != 0) {                                                  /* the member could not be read: say why */
            bool unsupported = rc == -EOPNOTSUPP || rc == -EACCES;
            const LangStrings *lang = i18n_get();
            App_set_status(app, unsupported ? lang->s[LS_arc_member_unsupported] : lang->s[LS_arc_member_bad], MSG_ERR);
            return;
        }
    } else {
        const char *name = DirModel_entry_name(m, fe);
        if (path_join(full, m->path, name, PATH_MAX) < 0) {
            App_set_status(app, fs_errno_message(-ENAMETOOLONG), MSG_ERR);
            return;
        }
        if (m->virtual_mode == 3) {
            if (pkg_vfs_read_uri((char **)(uintptr_t)app->envp, full, full, PATH_MAX) != 0) return;
        }
    }
    App_open_viewer(app, full);
}

void App_cmd_edit(App *app)
{
    FilePane *pane = (FilePane *)(uintptr_t)app->active_pane;
    FileEntry *fe = FilePane_get_selected_entry(pane);
    if (!fe) return;
    if (fe->flags & (FE_IS_DIR | FE_LINK_DIR)) {
        App_set_status(app, i18n_get()->s[LS_app_err_isdir], MSG_WARN);
        return;
    }
    char full[PATH_MAX];
    DirModel *m = (DirModel *)(uintptr_t)pane->model;
    if (path_join(full, m->path, DirModel_entry_name(m, fe), PATH_MAX) < 0) {
        App_set_status(app, fs_errno_message(-ENAMETOOLONG), MSG_ERR);
        return;
    }
    (void)sys_exec_sh_arg("exec \"${EDITOR:-vi}\" \"$1\"", full, (char **)(uintptr_t)app->envp);
    ScreenBuffer *scr = (ScreenBuffer *)(uintptr_t)app->screen;
    scr->force_full = 1;
    reload_both_panes(app, nullptr);
}

void App_cmd_shell(App *app)
{
    char cwd[PATH_MAX];
    if (os_getcwd(cwd, PATH_MAX) < 0) cwd[0] = '\0';
    FilePane *pane = (FilePane *)(uintptr_t)app->active_pane;
    DirModel *m = (DirModel *)(uintptr_t)pane->model;
    (void)os_chdir(m->path);
    (void)sys_exec_sh_arg("exec \"${SHELL:-/bin/sh}\"", nullptr, (char **)(uintptr_t)app->envp);
    if (cwd[0] != '\0') (void)os_chdir(cwd);
    ScreenBuffer *scr = (ScreenBuffer *)(uintptr_t)app->screen;
    scr->force_full = 1;
    reload_both_panes(app, nullptr);
}

/* an archive pane lists a virtual folder: its DirModel.path is the archive file, so no copy / move / delete can address its entries */
static bool refuse_in_archive(App *app, const DirModel *m)
{
    if (m->virtual_mode != 2) return false;
    App_set_status(app, i18n_get()->s[LS_arc_readonly], MSG_WARN);
    return true;
}

static void open_transfer_dlg(App *app, uint32_t op_tag)
{
    FilePane *pane = (FilePane *)(uintptr_t)app->active_pane;
    DirModel *m = (DirModel *)(uintptr_t)pane->model;
    if (refuse_in_archive(app, m)) return;
    if (DirModel_count_selected(m, nullptr) == 0) {
        FileEntry *fe = FilePane_get_selected_entry(pane);
        if (!fe) return;
        if (fe->flags & FE_IS_PARENT) {
            App_set_status(app, i18n_get()->s[LS_app_err_parent], MSG_WARN);
            return;
        }
    }
    FilePane *inact = get_inactive_pane(app);
    DirModel *im = (DirModel *)(uintptr_t)inact->model;
    const LangStrings *lang = i18n_get();
    int is_mv = (op_tag == OP_MOVE);
    const char *title = is_mv ? lang->s[LS_app_title_move] : lang->s[LS_app_title_copy];
    const char *prompt = is_mv ? lang->s[LS_app_prm_move] : lang->s[LS_app_prm_copy];
    open_input_dlg(app, title, prompt, im->path, op_tag);
}

void App_cmd_copy(App *app) { open_transfer_dlg(app, OP_COPY); }
void App_cmd_move(App *app) { open_transfer_dlg(app, OP_MOVE); }

void App_cmd_delete(App *app)
{
    FilePane *pane = (FilePane *)(uintptr_t)app->active_pane;
    DirModel *m = (DirModel *)(uintptr_t)pane->model;
    if (refuse_in_archive(app, m)) return;
    size_t cnt = DirModel_count_selected(m, nullptr);
    const LangStrings *lang = i18n_get();
    if (cnt != 0) {
        char buf[128];
        buf[u64_to_dec((uint64_t)cnt, buf)] = '\0';
        (void)str_cat(buf, lang->s[LS_app_sel_items_sfx], sizeof buf);
        app->active_dialog = (uint64_t)(uintptr_t)ConfirmDialog_new(
            app, lang->s[LS_app_title_delete], lang->s[LS_app_msg_del_many], buf, OP_DELETE);
        app->need_redraw = 1;
        return;
    }
    FileEntry *fe = FilePane_get_selected_entry(pane);
    if (!fe) return;
    if (fe->flags & FE_IS_PARENT) {
        App_set_status(app, lang->s[LS_app_err_parent], MSG_WARN);
        return;
    }
    const char *name = DirModel_entry_name(m, fe);
    app->active_dialog = (uint64_t)(uintptr_t)ConfirmDialog_new(
        app, lang->s[LS_app_title_delete], lang->s[LS_app_msg_del_one], name, OP_DELETE);
    app->need_redraw = 1;
}
