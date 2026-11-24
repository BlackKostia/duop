/* src/app/app_arc.c - App/FilePane glue for Archive VFS navigation and extraction
 * (freestanding C23; replaces src/app/app_arc.s, kept as tests/harness/app_arc_reference.s). */
#include "duop_app.h"
#include "duop_app_int.h"
#include "duop_arc.h"
#include "duop_classes.h"
#include "duop_fs.h"
#include "duop_tui.h"
#include "duop_ui.h"
#include "i18n_slots.h"

int arc_nav_up(FilePane *fp)
{
    int idx = arc_nav_up_model((DirModel *)(uintptr_t)fp->model);
    if (idx < 0) return 0;
    fp->cursor = (uint32_t)idx;
    fp->scroll = 0;
    FilePane_ensure_visible(fp);
    return 1;
}

int arc_handle_enter(App *app)
{
    FilePane *ap = (FilePane *)(uintptr_t)app->active_pane;
    FileEntry *e = FilePane_get_selected_entry(ap);
    int r = arc_enter_model((DirModel *)(uintptr_t)ap->model, e);
    if (r == 0) return 0;
    if (r >= 4) {                                                    /* an archive that cannot be entered: say why instead of opening it as a file */
        const LangStrings *lang = i18n_get();
        App_set_status(app, r == 5 ? lang->s[LS_arc_too_big] : r == 6 ? lang->s[LS_arc_format_unsupported] : lang->s[LS_arc_not_archive], MSG_WARN);
        app->need_redraw = 1;
        return 1;
    }
    if (r == 2) {
        (void)arc_nav_up(ap);
    } else {
        ap->cursor = 0;
        ap->scroll = 0;
        if (r == 3) App_set_status(app, i18n_get()->s[LS_arc_truncated], MSG_OK);
    }
    app->need_redraw = 1;
    return 1;
}

int arc_extract_selected(FilePane *fp, char *out_tmp_path)
{
    FileEntry *e = FilePane_get_selected_entry(fp);
    return (int)arc_extract_entry((DirModel *)(uintptr_t)fp->model, e, out_tmp_path);   /* 0 or -errno */
}
