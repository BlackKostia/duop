/* src/app/app_ext.c - Extended App dialog close & keybind routing
 * (freestanding C23; replaces src/app/app_ext.s and src/app/app_ext_keys.s,
 * kept as tests/harness/app_ext{,_keys}_reference.s). */
#include "duop_app.h"
#include "duop_app_int.h"
#include "duop_classes.h"
#include "duop_config.h"
#include "duop_fs.h"
#include "duop_keys.h"
#include "duop_pkg.h"
#include "duop_string.h"
#include "duop_tui.h"
#include "duop_ui.h"
#include "i18n_slots.h"

#define EXT_NAME_MAX 256

void app_jump_virtual_entry(App *app, FilePane *pane, const char *full_path)
{
    char dir[PATH_MAX];
    char base[EXT_NAME_MAX];
    (void)str_copy(dir, full_path, sizeof dir);
    const char *bn = path_basename(dir);
    (void)str_copy(base, bn, sizeof base);
    char *slash = (char *)bn - 1;
    if (bn <= dir) {
        dir[0] = '/'; dir[1] = '\0';
    } else if (slash == dir) slash[1] = '\0';
    else slash[0] = '\0';

    DirModel *m = (DirModel *)(uintptr_t)pane->model;
    m->virtual_mode = 0;
    if (FilePane_nav_to(pane, dir) < 0) return;
    intptr_t idx = DirModel_find_by_name(m, base);
    if (idx >= 0) {
        pane->cursor = (uint32_t)idx;
        FilePane_ensure_visible(pane);
    }
    App_sync_preview(app);
    app->need_redraw = 1;
}

static void app_close_fuzzy_dlg(App *app, FuzzyDialog *dlg, FilePane *pane)
{
    uintptr_t sel = FuzzyDialog_get_selected(dlg);
    if (!sel) return;
    if (dlg->base.op_tag == OP_PALETTE) {
        if (sel >= 0x1000u) {
            PkgAction *act = pkg_menu_get((int)(sel - 0x1000u));
            if (act) App_run_pkg_action(app, act);
        } else {
            App_dispatch_action(app, (int)sel);
        }
        return;
    }
    char full[PATH_MAX];
    if (path_join(full, (const char *)dlg->base_path, (const char *)sel, PATH_MAX) < 0) return;
    app_jump_virtual_entry(app, pane, full);
}

static void app_close_usermenu_dlg(App *app, UserMenuDialog *dlg)
{
    int cnt = config_menu_count();
    int cur = (int)dlg->cursor;
    if (cur < cnt) {
        const char *lbl = nullptr, *cmd = nullptr;
        if (config_menu_item(cur, &lbl, &cmd)) App_exec_menu_tmpl(app, cmd);
        return;
    }
    PkgAction *act = pkg_menu_get(cur - cnt);
    if (act) App_run_pkg_action(app, act);
}

static void app_close_search_dlg(App *app, SearchDialog *dlg, FilePane *pane, DirModel *model)
{
    int hits = dir_search_run(model, model->path, (const char *)dlg->name_buf, (const char *)dlg->grep_buf);
    uint32_t cur = 0;
    if (hits > 0) {
        DirModel *m = (DirModel *)(uintptr_t)pane->model;
        DirModel_sort(m);
        if (m->has_dotdot && m->count > 1) cur = 1;
    }
    pane->cursor = cur;
    pane->scroll = 0;
    FilePane_ensure_visible(pane);
    App_sync_preview(app);
    if (hits == 0)
        App_set_status(app, i18n_get()->s[LS_msg_search_none], MSG_INFO);
}

intptr_t App_close_dialog_ext(App *app, Dialog *dlg, FilePane *pane, DirModel *model, int dlg_res)
{
    switch (dlg->op_tag) {
    case OP_USERMENU: app_close_usermenu_dlg(app, (UserMenuDialog *)dlg); break;
    case OP_BOOKMARKS:
    case OP_FRECENCY: {
        const char *sel = BookmarkDialog_get_selected((BookmarkDialog *)dlg);
        if (!sel) break;
        intptr_t rc = FilePane_nav_to(pane, sel);
        if (rc < 0) return rc;
        App_sync_preview(app);
        break;
    }
    case OP_CONFLICT: App_task_conflict_done(app, dlg_res, (ConflictDialog *)dlg); break;
    case OP_SEARCH: app_close_search_dlg(app, (SearchDialog *)dlg, pane, model); break;
    case OP_CHMOD: App_apply_chmod(app, (ChmodDialog *)dlg); break;
    case OP_SYMLINK: App_apply_link(app, (InputDialog *)dlg, 0); break;
    case OP_HARDLINK: App_apply_link(app, (InputDialog *)dlg, 1); break;
    case OP_PALETTE:
    case OP_FUZZY: app_close_fuzzy_dlg(app, (FuzzyDialog *)dlg, pane); break;
    case OP_VIEWAS: App_view_as_done(app, (ViewAsDialog *)dlg); break;
    case OP_UNPACK: App_unpack_confirm(app, (const char *)((InputDialog *)dlg)->buf); break;
    default: break;
    }
    return 0;
}

static int app_open_fuzzy(App *app, int mode)
{
    FilePane *pane = (FilePane *)(uintptr_t)app->active_pane;
    DirModel *m = (DirModel *)(uintptr_t)pane->model;
    FuzzyDialog *d = FuzzyDialog_new(app, mode, m->path);
    if (d) {
        app->active_dialog = (uint64_t)(uintptr_t)d;
        app->need_redraw = 1;
    }
    return 1;
}

static int app_cmd_diff(App *app)
{
    FilePane *act = (FilePane *)(uintptr_t)app->active_pane;
    FilePane *right = (FilePane *)(uintptr_t)app->right_pane;
    FilePane *other = (act == right) ? (FilePane *)(uintptr_t)app->left_pane : right;
    int cnt = dir_diff_mark((DirModel *)(uintptr_t)act->model, (DirModel *)(uintptr_t)other->model);
    char buf[32], num[32];
    (void)str_copy(buf, i18n_get()->s[LS_diff_pfx], sizeof buf);
    num[u64_to_dec((uint64_t)(uint32_t)cnt, num)] = '\0';
    (void)str_cat(buf, num, sizeof buf);
    App_set_status(app, buf, MSG_INFO);
    app->need_redraw = 1;
    return 1;
}

static int app_hist_move(App *app, int fwd)
{
    FilePane *pane = (FilePane *)(uintptr_t)app->active_pane;
    int ok = fwd ? FilePane_hist_fwd(pane) : FilePane_hist_back(pane);
    if (!ok) {
        App_set_status(app, i18n_get()->s[LS_msg_hist_empty], MSG_INFO);
        return 1;
    }
    App_sync_preview(app);
    app->need_redraw = 1;
    return 1;
}

static int handle_ext_ctrl(App *app, int key)
{
    if (key == 'f') {
        SearchDialog *d = SearchDialog_new(app);
        if (d) { app->active_dialog = (uint64_t)(uintptr_t)d; app->need_redraw = 1; }
        return 1;
    }
    if (key == 'z') { App_cmd_undo(app); return 1; }
    if (key == 'k') return app_open_fuzzy(app, 0);
    if (key == '`') { term_toggle(app); App_layout(app); return 1; }
    return 0;
}

static int handle_ext_alt(App *app, int key, int mods)
{
    int lc = key | 0x20;
    if (key == KEY_F9) {                                            /* Alt+F9 unpack, Alt+Shift+F9 test */
        if (mods & MOD_SHIFT) App_cmd_arctest(app);
        else App_cmd_unpack(app);
        return 1;
    }
    if (lc == 'p') return app_open_fuzzy(app, 1);
    if (lc == 'a') { App_cmd_chmod(app); return 1; }
    if (lc == 'v') return App_cmd_view_mode(app);
    if (lc == 'i') { App_cmd_info(app); return 1; }
    if (lc == 's') { App_cmd_link(app, 0); return 1; }
    if (lc == 'h') { App_cmd_link(app, 1); return 1; }
    if (lc == 'd') { du_toggle(app); return 1; }
    if (lc == 'b') {
        BulkRenameDialog *d = BulkRenameDialog_new(app, (FilePane *)(uintptr_t)app->active_pane);
        if (d) { app->active_dialog = (uint64_t)(uintptr_t)d; app->need_redraw = 1; }
        return 1;
    }
    if (lc == 'k') return App_cmd_packages(app);
    if (key == '=') return app_cmd_diff(app);
    if (key == KEY_LEFT) return app_hist_move(app, 0);
    if (key == KEY_RIGHT) return app_hist_move(app, 1);
    return 0;
}

int app_handle_ext_key(App *app, int key, int mods)
{
    if (mods & MOD_CTRL) return handle_ext_ctrl(app, key);
    if (mods & MOD_ALT) return handle_ext_alt(app, key, mods);
    if (key == KEY_ENTER) return arc_handle_enter(app);
    return 0;
}
