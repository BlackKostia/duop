/* include/duop_ui.h - the C forms of the assembler OOP / TRY macros, for the C23 ports of src/ui and src/app
 * (include/oop.inc, classes.inc, try_using.inc; docs/HYBRID_PORTABILITY_PLAN.md phase P2).
 *
 *   asm                                  C
 *   NEW_OBJ vtbl, size                   DUOP_NEW(Type, vtbl)                   zeroed, vptr set, NULL on OOM (test it)
 *   DELETE_OBJ reg                       DUOP_DELETE(obj)                       dtor through the vtable, then mem_free; NULL-safe
 *   IS_INSTANCE reg, vtbl                DUOP_IS_INSTANCE(obj, vtbl)            1 for the class or a subclass
 *   VCALL slot                           WIDGET_CALL(obj, slot, args...)        also OBJECT_CALL, DIALOG_CALL, DUOP_VCALL
 *   SUPER_CALL parent_vtbl, slot         DUOP_SUPER_CALL(Vtbl, Self, parent_vtbl, obj, slot, args...)
 *   WIDGET_VTBL_SLOTS name, ...          WIDGET_VTBL(NAME, "Class", SUPER_VTBL, sizeof(Class), .slot = fn, ...)
 *   DIALOG_VTBL_SLOTS name, ...          DIALOG_VTBL(NAME, "Class", SUPER_VTBL, sizeof(Class), .slot = fn, ...)
 *   TRY_BEGIN / TRY_END                  DUOP_TRY_BEGIN(frame, sig) { ... DUOP_TRY_END(frame); }
 *
 * Vtables are `const` (read-only data), the first member of every vtable is its ObjectVtbl, and `Object.vptr` is at offset 0, so a
 * subclass vtable converts to `const ObjectVtbl *` with a plain cast. Widget structs come from build/duop_classes.h (tools/gen_abi.py). */
#pragma once
#include "duop_abi.h"
#include "duop_oop.h"

/* StatusBar message kinds (classes.inc MSG_*) and the message size (config.inc STATUS_MAX) */
#define MSG_INFO 0
#define MSG_OK 1
#define MSG_WARN 2
#define MSG_ERR 3
#define STATUS_MAX 512
#define INPUT_MAX 512

/* Dialog.result / App_close_dialog codes (include/config.inc) */
#define DLG_OPEN 0
#define DLG_OK 1
#define DLG_CANCEL 2

/* Right-pane mode (include/config.inc) */
#define RIGHT_MODE_PANE 0
#define RIGHT_MODE_PREVIEW 1

/* Leaf UI helpers (src/ui/) */
void osc52_copy_text(const char *str);
int term_key_encode(const void *evt, char *out);
int term_on_key(void *app, void *evt);
void term_shutdown(void);
int PreviewPane_on_mouse(Widget *self, void *evt);
void hdr_draw_mode_and_git(void *hdr, void *scr, const void *theme, const void *ls);

typedef struct FilePane FilePane;
typedef struct FileEntry FileEntry;
typedef struct FilePaneCols {
    int32_t name_w;
    int32_t size_w;
    int32_t extra_w;
    int32_t extra_is_date;
} FilePaneCols;
FilePaneCols file_pane_compute_cols(int32_t w);
void FilePane_layout(Widget *self, int x, int y, int w, int h);
void FilePane_ensure_visible(FilePane *self);
FileEntry *FilePane_get_selected_entry(FilePane *self);

typedef struct ScreenBuffer ScreenBuffer;
typedef struct Theme Theme;
typedef struct PreviewPane PreviewPane;
int syntax_detect(const char *path);
void syntax_draw_line(ScreenBuffer *scr, int x, int y, const char *line, int max_w,
                      const char *path, const Theme *theme, uint32_t bg);
int elf_preview_try_render(PreviewPane *pv, ScreenBuffer *scr, const Theme *theme);
int InputDialog_on_key(Widget *self, void *event);

typedef struct FileEntry FileEntry;
void fp_draw_glyph_and_git(ScreenBuffer *scr, int x3, int y, uint32_t glyph,
                           FilePane *fp, FileEntry *ent, const Theme *theme, uint32_t row_bg);
void hdr_draw_git_badge(void *hdr, void *scr, const void *theme, int x, int y);
void fp_draw_du_badge(FilePane *fp, ScreenBuffer *sb);
void fp_draw_du_pane(FilePane *fp, ScreenBuffer *sb, const Theme *th);

typedef struct App App;
int term_height(int main_h);
void term_toggle(App *app);
void term_poll_read(App *app);
void term_render(App *app, ScreenBuffer *scr, const Theme *theme);
typedef struct FunctionBar FunctionBar;
extern const WidgetVtbl FunctionBar_vtbl;
FunctionBar *FunctionBar_new(App *app);
void FunctionBar_layout(Widget *self, int x, int y, int w, int h);
void FunctionBar_render(Widget *self, void *screen, void *theme);
int FunctionBar_on_mouse(Widget *self, void *event);
enum {
    HDR_BTN_THEME = 1, HDR_BTN_PREVIEW = 2, HDR_BTN_HIDDEN = 3,
    HDR_BTN_HELP = 4, HDR_BTN_QUIT = 5, HDR_BTN_LANG = 6,
};
typedef struct HeaderBar HeaderBar;
extern const WidgetVtbl HeaderBar_vtbl;
HeaderBar *HeaderBar_new(App *app);
void HeaderBar_layout(Widget *self, int x, int y, int w, int h);
void HeaderBar_render(Widget *self, void *screen, void *theme);
int HeaderBar_on_mouse(Widget *self, void *event);
typedef struct NavTab NavTab;
NavTab *fp_get_active_tab(FilePane *self);
intptr_t FilePane_nav_to(FilePane *self, const char *path);
int FilePane_hist_back(FilePane *self);
int FilePane_hist_fwd(FilePane *self);
int FilePane_tab_new(FilePane *self);
int FilePane_tab_switch(FilePane *self, int idx);
int FilePane_tab_close(FilePane *self, int idx);
int get_list_h(FilePane *self);
void FilePane_enter_parent(FilePane *self);
int FilePane_on_key(Widget *self, void *event);
void scrub_scrollbar(FilePane *self, int my);
#define DBLCLICK_MS 420
int FilePane_on_mouse(Widget *self, void *event);
typedef struct PopupMenuDialog PopupMenuDialog;
typedef struct Event Event;
PopupMenuDialog *PopupMenu_new(App *app, int x, int y);
int pane_mouse_ext(App *app, Event *evt);
#define CH_BOX_LT 0x251Cu
#define CH_BOX_RT 0x2524u
#define CH_TRI_RIGHT 0x25B8u
#define CH_SQ_SMALL 0x25AAu
#define CH_DIAMOND 0x25C6u
#define CH_ARROW_NE 0x2197u
#define CH_CHECK 0x2713u
#define CH_LAQUO 0x00ABu
#define CH_ELLIPSIS 0x2026u
void fp_render_rows(FilePane *fp, ScreenBuffer *scr, const Theme *th,
                    const FilePaneCols *cols, int list_h, int focused);
void FilePane_render(Widget *self, void *screen, void *theme);
extern const WidgetVtbl FilePane_vtbl;
FilePane *FilePane_new(App *app, const char *initial_path, int id);
void FilePane_dtor(Widget *self);
void FilePane_reload_keep_cursor(FilePane *self, const char *focus_name);
typedef struct ImgPlace ImgPlace;
void img_overlay_show(const ImgPlace *p);
void img_preview_pre_flush(App *app);
void img_preview_post_flush(App *app);
int img_overlay_kitty_ready(const void *pixels);
void img_overlay_kitty_mark(const void *pixels);

typedef struct ImgView {
    const uint8_t *rgba;
    int32_t iw;
    int32_t ih;
    uint32_t bg;
} ImgView;
uint32_t img_blend(uint32_t rgba, uint32_t bg);
void img_draw_rgba(ScreenBuffer *scr, int bx, int by, int bw, int bh, const ImgView *v);
void img_draw_quad(ScreenBuffer *scr, int bx, int by, int bw, int bh, const ImgView *v);
void img_draw_braille(ScreenBuffer *scr, int bx, int by, int bw, int bh, const ImgView *v);
int img_preview_try_render(PreviewPane *pv, ScreenBuffer *sb, const Theme *th);
typedef struct PvThumbReq PvThumbReq;
uint32_t PreviewPane_thumb_req(PreviewPane *self, PvThumbReq *out);
void PreviewPane_cycle_mode(PreviewPane *self);
int pv_mode_dispatch(PreviewPane *self, ScreenBuffer *scr, const Theme *theme);
extern const WidgetVtbl PreviewPane_vtbl;
PreviewPane *PreviewPane_new(App *app);
int PreviewPane_load(PreviewPane *self, const char *full_path, const FileEntry *entry);
void PreviewPane_read_sync(PreviewPane *self);
void PreviewPane_accept(PreviewPane *self, size_t len, size_t tlen, const uint8_t *thumb);
void PreviewPane_render(Widget *self, void *screen, void *theme);
const char *pv_copy_line_strip_ansi(const char *p, const char *end, char *dst);

#define CH_BLOCK_FULL 0x2588u
#define CH_SHADE_LIGHT 0x2591u
typedef struct ViewerDialog ViewerDialog;
typedef struct Event Event;
void vw_draw_img_scrollbars(ViewerDialog *v, ScreenBuffer *scr, const Theme *th);
void vw_draw_text_scrollbar(ViewerDialog *v, ScreenBuffer *scr, const Theme *th, int64_t end_off);
int vw_img_scrollbar_mouse(ViewerDialog *v, Event *e);
void vz_invalidate(ViewerDialog *v);
int vim_zoom_key(ViewerDialog *v, Event *e);
int vim_zoom_mouse(ViewerDialog *v, Event *e);
void vim_zoom_rect(ViewerDialog *v, int box_w, int box_h, int upscale,
                   int *out_tw, int *out_th, int *out_rect);
void vim_draw_zoom_badge(ViewerDialog *v, ScreenBuffer *scr, const Theme *th);
int vim_kitty_pan(ViewerDialog *v, int proto, ImgPlace *p);
void vim_place(ViewerDialog *v, ImgPlace *p);
void vim_draw_picture(ViewerDialog *v, ScreenBuffer *scr, const Theme *theme);
void ViewerDialog_on_btn0(ViewerDialog *v);
void ViewerDialog_set_mode(ViewerDialog *v, int mode);
void ViewerDialog_render_image(ViewerDialog *v, ScreenBuffer *scr);
int viewer_mode_key(ViewerDialog *v, Event *e);
void viewer_img_free(ViewerDialog *v);
void hex_edit_free(ViewerDialog *vd);
int hex_edit_on_key(ViewerDialog *vd, Event *ev);

typedef struct Dialog Dialog;

/* DialogVtbl (classes.inc): WidgetVtbl + the two button actions. Dialog_* defaults live in src/ui/dialogs*.s until Dialog is ported. */
typedef struct DialogVtbl {
    WidgetVtbl base;
    void (*on_confirm)(Dialog *self);
    void (*on_cancel)(Dialog *self);
} DialogVtbl;

void Dialog_dtor(Widget *self);
void Dialog_layout(Widget *self, int x, int y, int w, int h);
void Dialog_render(Widget *self, void *screen, void *theme);
int Dialog_on_key(Widget *self, void *event);
int Dialog_on_mouse(Widget *self, void *event);
void Dialog_on_confirm(Dialog *self);
void Dialog_on_cancel(Dialog *self);
extern const DialogVtbl Dialog_vtbl;
/* Dialog operation tags (classes.inc: OP_*) and viewer/preview presentation modes (config.inc: PV_MODE_*) */
enum {
    OP_NONE = 0, OP_MKDIR = 1, OP_RENAME = 2, OP_COPY = 3, OP_MOVE = 4, OP_DELETE = 5,
    OP_NEWFILE = 6, OP_FILTER = 7, OP_GOTO = 8, OP_CHMOD = 9, OP_PROGRESS = 10, OP_QUIT = 11,
    OP_USERMENU = 12, OP_BOOKMARKS = 13, OP_FRECENCY = 14, OP_CONFLICT = 15, OP_SEARCH = 16,
    OP_INFO = 17, OP_SYMLINK = 18, OP_HARDLINK = 19, OP_PALETTE = 20, OP_FUZZY = 21, OP_VIEWAS = 22, OP_UNPACK = 23,
};
enum { PV_MODE_AUTO = 0, PV_MODE_IMAGE = 1, PV_MODE_HEX = 2, PV_MODE_TEXT = 3 };
/* Configurable action IDs (config.inc: ACT_*) */
enum {
    ACT_QUIT = 1, ACT_HELP = 2, ACT_RENAME = 3, ACT_VIEW = 4, ACT_EDIT = 5, ACT_COPY = 6,
    ACT_MOVE = 7, ACT_MKDIR = 8, ACT_DELETE = 9, ACT_PREVIEW = 10, ACT_HIDDEN = 11,
    ACT_THEME = 12, ACT_LANG = 13, ACT_FILTER = 14, ACT_GOTO = 15, ACT_NEWFILE = 16,
    ACT_SHELL = 17, ACT_REFRESH = 18, ACT_USERMENU = 19,
};

extern const DialogVtbl ConfirmDialog_vtbl;
extern const DialogVtbl InputDialog_vtbl;
extern const DialogVtbl ConflictDialog_vtbl;
extern const DialogVtbl ChmodDialog_vtbl;
extern const DialogVtbl InfoDialog_vtbl;
extern const DialogVtbl ViewAsDialog_vtbl;
extern const DialogVtbl BookmarkDialog_vtbl;
extern const DialogVtbl SearchDialog_vtbl;
extern const DialogVtbl UserMenuDialog_vtbl;
extern const DialogVtbl BulkRenameDialog_vtbl;
extern const DialogVtbl ProgressDialog_vtbl;
extern const DialogVtbl DuProgressDialog_vtbl;
extern const DialogVtbl HelpDialog_vtbl;
extern const DialogVtbl PkgDialog_vtbl;
extern const DialogVtbl ViewerDialog_vtbl;
typedef struct ConfirmDialog ConfirmDialog;
typedef struct InputDialog InputDialog;
typedef struct ConflictDialog ConflictDialog;
typedef struct ChmodDialog ChmodDialog;
typedef struct InfoDialog InfoDialog;
typedef struct ViewAsDialog ViewAsDialog;
typedef struct BookmarkDialog BookmarkDialog;
typedef struct SearchDialog SearchDialog;
typedef struct FuzzyDialog FuzzyDialog;
typedef struct UserMenuDialog UserMenuDialog;
typedef struct BulkRenameDialog BulkRenameDialog;
typedef struct ProgressDialog ProgressDialog;
typedef struct HelpDialog HelpDialog;
typedef struct PkgDialog PkgDialog;
void Dialog_init(Dialog *self, App *app, const char *title, uint32_t op_tag);
void Dialog_draw_frame(Dialog *self, ScreenBuffer *scr, const Theme *theme, const char *b0_lbl, const char *b1_lbl);
void dlg_layout_center_buttons(Dialog *self, const char *btn0, const char *btn1);
void dlg_layout_right_buttons(Dialog *self, const char *btn0, const char *btn1);
ConfirmDialog *ConfirmDialog_new(App *app, const char *title, const char *message, const char *detail, uint32_t op_tag);
void ConfirmDialog_layout(Widget *self, int x, int y, int w, int h);
void ConfirmDialog_render(Widget *self, void *screen, void *theme);
InputDialog *InputDialog_new(App *app, const char *title, const char *prompt, const char *initial_text, uint32_t op_tag);
ConflictDialog *ConflictDialog_new(App *app, const char *name, uint64_t src_sz, uint64_t dst_sz);
ChmodDialog *ChmodDialog_new(App *app, const char *path);
InfoDialog *InfoDialog_new(App *app, const char *path);
ViewAsDialog *ViewAsDialog_new(App *app, const char *path, int fmt);
BookmarkDialog *BookmarkDialog_new(App *app, int mode);
const char *BookmarkDialog_get_selected(BookmarkDialog *self);
SearchDialog *SearchDialog_new(App *app);
int arc_nav_up(FilePane *fp);
int fp_exit_virtual(FilePane *pane);
void fp_draw_search_badge(FilePane *pane, ScreenBuffer *scr);
FuzzyDialog *FuzzyDialog_new(App *app, int mode, const char *base_path);
uintptr_t FuzzyDialog_get_selected(FuzzyDialog *self);
UserMenuDialog *UserMenuDialog_new(App *app);
BulkRenameDialog *BulkRenameDialog_new(App *app, FilePane *pane);
ProgressDialog *ProgressDialog_new(App *app);
void ProgressDialog_layout(Widget *self, int x, int y, int w, int h);
const char *ProgressDialog_title_for(int op);
void ProgressDialog_format_summary(const void *snap, char *buf);
Dialog *DuProgressDialog_new(App *app);
void DuProgressDialog_layout(Widget *self, int x, int y, int w, int h);
HelpDialog *HelpDialog_new(App *app);
PkgDialog *PkgDialog_new(App *app);
int App_cmd_packages(App *app);
void App_pkg_install_confirm(App *app, InputDialog *dlg);
ViewerDialog *ViewerDialog_new(App *app, const char *path);
void ViewerDialog_render(Widget *self, void *screen, void *theme);
typedef struct StatusBar StatusBar;
StatusBar *StatusBar_new(App *app);
void StatusBar_set_message(StatusBar *self, const char *msg, int kind, int ttl_ms);

/* ---- allocation, destruction, run-time type checks ---- */
#define DUOP_OBJ_VTBL(vtbl) ((const ObjectVtbl *)&(vtbl))
#define DUOP_NEW(Type, vtbl) ((Type *)Object_alloc(DUOP_OBJ_VTBL(vtbl), sizeof(Type)))
#define DUOP_DELETE(obj) Object_free((Object *)(obj))
#define DUOP_IS_INSTANCE(obj, vtbl) Object_is_instance((const Object *)(obj), DUOP_OBJ_VTBL(vtbl))

/* ---- virtual dispatch: the vtable of `obj` is read at run time, `this` is the first argument ---- */
#define DUOP_VCALL(Vtbl, Self, obj, slot, ...) \
    (((const Vtbl *)((const Object *)(obj))->vptr)->slot((Self *)(obj)__VA_OPT__(, ) __VA_ARGS__))
#define OBJECT_CALL(obj, slot, ...) DUOP_VCALL(ObjectVtbl, Object, obj, slot __VA_OPT__(, ) __VA_ARGS__)
#define WIDGET_CALL(obj, slot, ...) DUOP_VCALL(WidgetVtbl, Widget, obj, slot __VA_OPT__(, ) __VA_ARGS__)
#define DIALOG_CALL(obj, slot, ...) DUOP_VCALL(DialogVtbl, Dialog, obj, slot __VA_OPT__(, ) __VA_ARGS__)
#define DUOP_SUPER_CALL(Vtbl, Self, parent_vtbl, obj, slot, ...) \
    (((const Vtbl *)&(parent_vtbl))->slot((Self *)(obj)__VA_OPT__(, ) __VA_ARGS__))

/* ---- vtable builders: every slot starts at the base class default, `.slot = fn` overrides it (the later initialiser wins, which
 * is exactly what -Woverride-init warns about, so it is silenced around the definition). The macro ends in `;`, and a further
 * `;` after it is harmless. ---- */
#if defined(__clang__)
#define DUOP_VTBL_PUSH _Pragma("clang diagnostic push") _Pragma("clang diagnostic ignored \"-Winitializer-overrides\"")
#define DUOP_VTBL_POP _Pragma("clang diagnostic pop")
#else
#define DUOP_VTBL_PUSH _Pragma("GCC diagnostic push") _Pragma("GCC diagnostic ignored \"-Woverride-init\"")
#define DUOP_VTBL_POP _Pragma("GCC diagnostic pop")
#endif

#define WIDGET_VTBL(NAME, CLASS, SUPER_VTBL, INST_SIZE, ...)                                                                           \
    DUOP_VTBL_PUSH const WidgetVtbl NAME = {                                                                                           \
        .base = {DUOP_OBJ_VTBL(SUPER_VTBL), CLASS, (INST_SIZE), (void (*)(Object *))Widget_dtor},                                       \
        .layout = Widget_layout, .render = Widget_render, .on_key = Widget_on_key, .on_mouse = Widget_on_mouse,                        \
        .on_focus = Widget_on_focus, .hit_test = Widget_hit_test, __VA_ARGS__};                                                                                           \
    DUOP_VTBL_POP

#define DIALOG_VTBL(NAME, CLASS, SUPER_VTBL, INST_SIZE, ...)                                                                           \
    DUOP_VTBL_PUSH const DialogVtbl NAME = {                                                                                           \
        .base = {.base = {DUOP_OBJ_VTBL(SUPER_VTBL), CLASS, (INST_SIZE), (void (*)(Object *))Dialog_dtor},                              \
                 .layout = Dialog_layout, .render = Dialog_render, .on_key = Dialog_on_key, .on_mouse = Dialog_on_mouse,               \
                 .on_focus = Widget_on_focus, .hit_test = Widget_hit_test},                                                            \
        .on_confirm = Dialog_on_confirm, .on_cancel = Dialog_on_cancel, __VA_ARGS__};                                                         \
    DUOP_VTBL_POP

/* ---- hardware-fault guard (os_try_begin / os_try_end, pal_fault.s): `sig` is 0 when the block ran to the end, else the signal that
 * interrupted it. Locals written inside the block and read after a fault must be volatile; leave the block only through
 * DUOP_TRY_END (a `return` or `goto` out of it leaves the frame linked). ---- */
#define DUOP_TRY_BEGIN(frame, sig) \
    TryFrame frame;                \
    int sig = os_try_begin(&(frame)); \
    if ((sig) == 0)
#define DUOP_TRY_END(frame) os_try_end(&(frame))
