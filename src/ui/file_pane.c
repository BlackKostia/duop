/* src/ui/file_pane.c - FilePane widget class: lifecycle, viewport clamping, selection
 * stat lookup, cursor-preserving reload, and parent navigation
 * (freestanding C23; replaces src/ui/file_pane.s, kept as tests/harness/file_pane_reference.s). */
#include "duop_arc.h"
#include "duop_classes.h"
#include "duop_du.h"
#include "duop_fs.h"
#include "duop_string.h"
#include "duop_tui.h"
#include "duop_ui.h"

#define FP_NAME_MAX 256

WIDGET_VTBL(FilePane_vtbl, "FilePane", Widget_vtbl, sizeof(FilePane),
            .base.dtor = (void (*)(Object *))FilePane_dtor,
            .layout = FilePane_layout, .render = FilePane_render,
            .on_key = FilePane_on_key, .on_mouse = FilePane_on_mouse);

FilePane *FilePane_new(App *app, const char *initial_path, int id)
{
    FilePane *fp = DUOP_NEW(FilePane, FilePane_vtbl);
    if (!fp) return nullptr;
    Widget_init(&fp->base, app, id);
    fp->app = (uint64_t)(uintptr_t)app;
    fp->last_click_idx = (uint32_t)-1;
    fp->hovered_row = (uint32_t)-1;
    fp->hovered_col_hdr = (uint32_t)-1;

    DirModel *m = DirModel_new(initial_path);
    if (!m) {
        mem_free(fp);
        return nullptr;
    }
    fp->model = (uint64_t)(uintptr_t)m;
    fp->tabs = (uint64_t)(uintptr_t)nav_tabs_alloc(m->path);
    fp->tab_count = 1;
    fp->active_tab = 0;
    return fp;
}

void FilePane_dtor(Widget *self)
{
    FilePane *fp = (FilePane *)self;
    nav_tabs_free((NavTab *)(uintptr_t)fp->tabs, (int)fp->tab_count);
    fp->tabs = 0;
    DUOP_DELETE((Object *)(uintptr_t)fp->model);
    fp->model = 0;
}

int get_list_h(FilePane *self)
{
    int h = self->base.h - 5;
    return h >= 1 ? h : 1;
}

void FilePane_ensure_visible(FilePane *self)
{
    DirModel *m = (DirModel *)(uintptr_t)self->model;
    uint64_t cnt = m->count;
    if (cnt == 0) {
        self->cursor = 0;
        self->scroll = 0;
        return;
    }
    int cur = (int32_t)self->cursor;
    if (cur < 0) cur = 0;
    if ((uint64_t)(uint32_t)cur >= cnt) cur = (int32_t)cnt - 1;
    self->cursor = (uint32_t)cur;

    int lh = get_list_h(self);
    int scr = (int32_t)self->scroll;
    if (cur < scr) scr = cur;
    else if (cur >= scr + lh) scr = cur - lh + 1;

    int max_scr = (int32_t)cnt - lh;
    if (max_scr < 0) max_scr = 0;
    if (scr > max_scr) scr = max_scr;
    if (scr < 0) scr = 0;
    self->scroll = (uint32_t)scr;
}

FileEntry *FilePane_get_selected_entry(FilePane *self)
{
    DirModel *m = (DirModel *)(uintptr_t)self->model;
    FileEntry *e = DirModel_get_entry(m, (int32_t)self->cursor);
    if (e) DirModel_ensure_entry_stat(m, e);
    return e;
}

void FilePane_reload_keep_cursor(FilePane *self, const char *focus_name)
{
    DirModel *m = (DirModel *)(uintptr_t)self->model;
    char saved[FP_NAME_MAX];
    const char *target = nullptr;
    if (focus_name) {
        (void)str_copy(saved, focus_name, FP_NAME_MAX);
        target = saved;
    } else {
        FileEntry *e = FilePane_get_selected_entry(self);
        if (e) {
            (void)str_copy(saved, DirModel_entry_name(m, e), FP_NAME_MAX);
            target = saved;
        }
    }
    if (m->virtual_mode == 2) arc_populate(m);                  /* an archive folder is rebuilt from its index, not read from a directory */
    else (void)DirModel_reload(m);
    if (target) {
        intptr_t idx = DirModel_find_by_name(m, target);
        if (idx >= 0) self->cursor = (uint32_t)idx;
    }
    FilePane_ensure_visible(self);
}

void FilePane_enter_parent(FilePane *self)
{
    if (fp_exit_virtual(self)) return;
    DirModel *m = (DirModel *)(uintptr_t)self->model;
    if (m->path[0] == '/' && m->path[1] == 0) return;

    char child[FP_NAME_MAX];
    (void)str_copy(child, path_basename(m->path), FP_NAME_MAX);
    if (DirModel_chdir(m, "..") < 0) return;
    if (m->du_mode) dir_du_populate(m);

    intptr_t idx = DirModel_find_by_name(m, child);
    self->cursor = (idx >= 0) ? (uint32_t)idx : 0;
    self->scroll = 0;
    FilePane_ensure_visible(self);

    NavTab *t = fp_get_active_tab(self);
    if (t) {
        nav_hist_push(t, m->path);
        nav_frecency_record(m->path);
    }
}
