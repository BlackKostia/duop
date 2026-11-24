/* src/app/app_open.c - Entry opening (VFS / directory / duoprc rule / viewer) & pane reload
 * (freestanding C23; replaces src/app/app_open.s, kept as tests/harness/app_open_reference.s). */
#include "duop_app.h"
#include "duop_app_int.h"
#include "duop_classes.h"
#include "duop_config.h"
#include "duop_fs.h"
#include "duop_oop.h"
#include "duop_pal.h"
#include "duop_pkg.h"
#include "duop_string.h"
#include "duop_ui.h"

static void open_parent_entry(App *app, FilePane *pane, char *full)
{
    DirModel *m = (DirModel *)(uintptr_t)pane->model;
    if (m->virtual_mode == 3) {
        if (pkg_vfs_parent_uri(m->path, full, PATH_MAX)) {
            (void)pkg_vfs_load_dir((char **)(uintptr_t)app->envp, m, full);
            pane->cursor = 0; pane->scroll = 0;
            FilePane_ensure_visible(pane);
        } else {
            void *saved = m->arc_ctx;
            m->virtual_mode = 0;
            m->arc_ctx = nullptr;
            if (saved) {
                (void)FilePane_nav_to(pane, (const char *)saved);
                mem_free(saved);
            }
        }
    } else {
        FilePane_enter_parent(pane);
    }
    App_sync_preview(app);
    app->need_redraw = 1;
}

static void open_dir_entry(App *app, FilePane *pane, const char *full)
{
    DirModel *m = (DirModel *)(uintptr_t)pane->model;
    if (m->virtual_mode == 3) {
        int rc = pkg_vfs_load_dir((char **)(uintptr_t)app->envp, m, full);
        if (rc < 0) { App_set_status(app, fs_errno_message(rc), MSG_ERR); return; }
        pane->cursor = 0; pane->scroll = 0;
        FilePane_ensure_visible(pane);
    } else {
        m->virtual_mode = 0;
        intptr_t rc = FilePane_nav_to(pane, full);
        if (rc < 0) { App_set_status(app, fs_errno_message((int)rc), MSG_ERR); return; }
    }
    App_sync_preview(app);
    app->need_redraw = 1;
}

void App_open_selected_entry(App *app, FilePane *pane)
{
    FileEntry *fe = FilePane_get_selected_entry(pane);
    if (!fe) return;
    if (arc_open_entry(app, pane)) return;                         /* an archive, a folder of one, ".." inside one: not a file to view */

    char full[PATH_MAX];
    DirModel *m = (DirModel *)(uintptr_t)pane->model;
    if (fe->flags & FE_IS_PARENT) {
        open_parent_entry(app, pane, full);
        return;
    }

    const char *name = DirModel_entry_name(m, fe);
    if (m->virtual_mode == 3 && name[0] == '.' && name[1] == '.' && name[2] == '\0') {
        open_parent_entry(app, pane, full);
        return;
    }
    if (path_join(full, m->path, name, PATH_MAX) < 0) {
        App_set_status(app, fs_errno_message(-ENAMETOOLONG), MSG_ERR);
        return;
    }
    if (fe->flags & (FE_IS_DIR | FE_LINK_DIR)) {
        open_dir_entry(app, pane, full);
        return;
    }
    if (m->virtual_mode == 1) { app_jump_virtual_entry(app, pane, full); return; }
    if (m->virtual_mode != 0) return;

    const char *tmpl = config_lookup_open(name);
    if (tmpl) { App_run_expanded_cmd(app, tmpl, name, full); return; }
    app->active_dialog = (uint64_t)(uintptr_t)ViewerDialog_new(app, full);
    app->need_redraw = 1;
}

void App_reload_panes(App *app, const char *focus_name)
{
    FilePane *act = (FilePane *)(uintptr_t)app->active_pane;
    FilePane_reload_keep_cursor(act, focus_name);
    FilePane_reload_keep_cursor(get_inactive_pane(app), nullptr);
    App_sync_preview(app);
    app->need_redraw = 1;
}

void reload_both_panes(App *app, const char *focus_name)
{
    App_reload_panes(app, focus_name);
}
