# Class Hierarchy & Virtual Table Specifications for Duop
.include "pal.inc"
# Inheritance tree:
#   Object
#     ├── ScreenBuffer
#     ├── DirModel
#     ├── App
#     └── Widget
#           ├── HeaderBar
#           ├── FunctionBar
#           ├── StatusBar
#           ├── FilePane
#           ├── PreviewPane
#           └── Dialog
#                 ├── ConfirmDialog
#                 ├── InputDialog
#                 ├── ViewerDialog
#                 ├── HelpDialog
#                 └── ProgressDialog
.intel_syntax noprefix
.include "config.inc"
.include "oop.inc"
.include "types.inc"
# ==============================================================================
# VTable Layouts
# ==============================================================================
VTBL_BEGIN ObjectVtbl
    VTBL_SLOT ObjectVtbl.super
    VTBL_SLOT ObjectVtbl.class_name
    VTBL_SLOT ObjectVtbl.inst_size
    VTBL_SLOT ObjectVtbl.dtor
VTBL_END ObjectVtbl
VTBL_BEGIN WidgetVtbl
    VTBL_EXTENDS ObjectVtbl
    VTBL_SLOT WidgetVtbl.layout
    VTBL_SLOT WidgetVtbl.render
    VTBL_SLOT WidgetVtbl.on_key
    VTBL_SLOT WidgetVtbl.on_mouse
    VTBL_SLOT WidgetVtbl.on_focus
    VTBL_SLOT WidgetVtbl.hit_test
VTBL_END WidgetVtbl

VTBL_BEGIN DialogVtbl
    VTBL_EXTENDS WidgetVtbl
    VTBL_SLOT DialogVtbl.on_confirm
    VTBL_SLOT DialogVtbl.on_cancel
VTBL_END DialogVtbl

# ==============================================================================
# Declarative VTable Builder Macros (Named Overrides + Inherited Defaults)
# ==============================================================================

.macro OBJECT_VTBL_SLOTS class_name_str, super_vtbl=0, inst_size=0, dtor=Object_dtor
    .quad \super_vtbl
    .quad \class_name_str
    .quad \inst_size
    .quad \dtor
.endm

.macro WIDGET_VTBL_SLOTS class_name_str, super_vtbl=Widget_vtbl, inst_size=Widget._size, dtor=Widget_dtor, layout=Widget_layout, render=Widget_render, on_key=Widget_on_key, on_mouse=Widget_on_mouse, on_focus=Widget_on_focus, hit_test=Widget_hit_test
    OBJECT_VTBL_SLOTS \class_name_str, \super_vtbl, \inst_size, \dtor
    .quad \layout
    .quad \render
    .quad \on_key
    .quad \on_mouse
    .quad \on_focus
    .quad \hit_test
.endm

.macro DIALOG_VTBL_SLOTS class_name_str, super_vtbl=Dialog_vtbl, inst_size=Dialog._size, dtor=Dialog_dtor, layout=Dialog_layout, render=Dialog_render, on_key=Dialog_on_key, on_mouse=Dialog_on_mouse, on_focus=Widget_on_focus, hit_test=Widget_hit_test, on_confirm=Dialog_on_confirm, on_cancel=Dialog_on_cancel
    WIDGET_VTBL_SLOTS \class_name_str, \super_vtbl, \inst_size, \dtor, \layout, \render, \on_key, \on_mouse, \on_focus, \hit_test
    .quad \on_confirm
    .quad \on_cancel
.endm

# ==============================================================================
# Class Struct Definitions
# ==============================================================================

CLASS_BEGIN Object
    FIELD Object.vptr, 8
CLASS_END Object

CLASS_BEGIN ScreenBuffer
    EXTENDS Object
    FIELD ScreenBuffer.front,      8
    FIELD ScreenBuffer.back,       8
    FIELD ScreenBuffer.out_buf,    8
    FIELD ScreenBuffer.out_len,    8
    FIELD ScreenBuffer.out_cap,    8
    FIELD ScreenBuffer.cell_cap,   8
    FIELD ScreenBuffer.width,      4
    FIELD ScreenBuffer.height,     4
    FIELD ScreenBuffer.force_full, 4
    FIELD ScreenBuffer.pen_fg,     4
    FIELD ScreenBuffer.pen_bg,     4
    FIELD ScreenBuffer.pen_attrs,  4
CLASS_END ScreenBuffer

CLASS_BEGIN DirModel
    EXTENDS Object
    FIELD DirModel.entries,        8   # compact FileEntry[] array (raw_cap * 32B)
    FIELD DirModel.order,          8   # uint32_t[] visible/sorted indices into entries[]
    FIELD DirModel.name_pool,      8   # contiguous NUL-terminated UTF-8 filename arena
    FIELD DirModel.count,          8   # visible filtered count (elements in order[])
    FIELD DirModel.raw_count,      8   # total loaded entries (elements in entries[])
    FIELD DirModel.raw_cap,        8   # capacity of entries[] and order[]
    FIELD DirModel.name_used,      8   # bytes used in name_pool
    FIELD DirModel.name_cap,       8   # byte capacity of name_pool
    FIELD DirModel.total_bytes,    8   # sum of bytes for stat'd non-dir entries
    FIELD DirModel.load_buf,       8   # DIRENTS_BUF_SIZE read buffer during streaming
    FIELD DirModel.load_pos,       8   # byte cursor inside load_buf
    FIELD DirModel.load_bytes,     8   # valid bytes in load_buf
    FIELD DirModel.load_dirfd,     4   # active O_DIRECTORY fd while streaming (or -1)
    FIELD DirModel.is_loading,     4   # 1 while incremental streaming active
    FIELD DirModel.load_aborted,   4   # 1 if user canceled streaming via Esc
    FIELD DirModel.huge_mode,      4   # 1 once raw_count >= g_huge_threshold
    FIELD DirModel.is_unsorted,    4   # 1 when in ls -f unsorted filesystem order
    FIELD DirModel.stats_complete, 4   # 1 when all raw entries have FE_STAT_READY
    FIELD DirModel.dir_count,      4
    FIELD DirModel.file_count,     4
    FIELD DirModel.sort_mode,      4
    FIELD DirModel.sort_desc,      4
    FIELD DirModel.show_hidden,    4
    FIELD DirModel.has_dotdot,     4   # 1 if entry 0 is '..'
    FIELD DirModel.dir_dev,        8   # identity + mtime of the listed directory at load
    FIELD DirModel.dir_ino,        8   # (validates LRU dir-cache snapshots)
    FIELD DirModel.dir_mtime,      8
    FIELD DirModel.dir_mtime_ns,   4
    FIELD DirModel.dir_pad,        4
    FIELD DirModel.flt_raw,        8   # raw_count when order[] was last filtered
    FIELD DirModel.flt_lc_len,     4   # byte length of flt_lc
    FIELD DirModel.flt_valid,      4   # 1: order[] == filter(flt_applied) over all raw entries
    FIELD DirModel.flt_hidden,     4   # show_hidden used for flt_applied
    FIELD DirModel.virtual_mode,   4   # 0=normal, 1=VIRTUAL_SEARCH
    FIELD DirModel.du_mode,        4   # 0=normal, 1=ncdu disk usage mode
    FIELD DirModel.du_total,       8   # total bytes in du_mode
    FIELD DirModel.arc_ctx,        8   # ArcCtx* when virtual_mode == 2
    FIELD DirModel.git_ctx,        8   # GitCtx* for git branch & file statuses
    FIELD DirModel.path,           PATH_MAX
    FIELD DirModel.filter,         FILTER_MAX
    FIELD DirModel.flt_lc,         FILTER_MAX  # ASCII-lowercased filter (SIMD needle)
    FIELD DirModel.flt_applied,    FILTER_MAX  # filter that produced the current order[]
CLASS_END DirModel

CLASS_BEGIN Widget
    EXTENDS Object
    FIELD Widget.parent, 8
    FIELD Widget.x,      4
    FIELD Widget.y,      4
    FIELD Widget.w,      4
    FIELD Widget.h,      4
    FIELD Widget.flags,  4
    FIELD Widget.id,     4
CLASS_END Widget

CLASS_BEGIN HeaderBar
    EXTENDS Widget
    FIELD HeaderBar.app,        8
    FIELD HeaderBar.hovered_id, 4
    FIELD HeaderBar.zone_count, 4
    FIELD HeaderBar.zones,      8*HitZone._size
CLASS_END HeaderBar

CLASS_BEGIN FunctionBar
    EXTENDS Widget
    FIELD FunctionBar.app,        8
    FIELD FunctionBar.hovered_id, 4
    FIELD FunctionBar.zone_count, 4
    FIELD FunctionBar.zones,      10*HitZone._size
CLASS_END FunctionBar

CLASS_BEGIN StatusBar
    EXTENDS Widget
    FIELD StatusBar.app,           8
    FIELD StatusBar.msg_expire_ms, 8
    FIELD StatusBar.msg_kind,      4
    FIELD StatusBar.filter_active, 4
    FIELD StatusBar.msg,           STATUS_MAX
CLASS_END StatusBar

CLASS_BEGIN FilePane
    EXTENDS Widget
    FIELD FilePane.model,           8
    FIELD FilePane.app,             8
    FIELD FilePane.last_click_ms,   8
    FIELD FilePane.last_click_idx,  4
    FIELD FilePane.cursor,          4
    FIELD FilePane.scroll,          4
    FIELD FilePane.hovered_row,     4
    FIELD FilePane.hovered_col_hdr, 4
    FIELD FilePane.dragging_scroll, 4
    FIELD FilePane.hdr_zone_count,  4
    FIELD FilePane.tab_count,       4
    FIELD FilePane.active_tab,      4
    FIELD FilePane.tab_zone_count,  4
    FIELD FilePane.tabs,            8
    FIELD FilePane.hdr_zones,       4*HitZone._size
    FIELD FilePane.tab_zones,       8*HitZone._size
CLASS_END FilePane

CLASS_BEGIN PreviewPane
    EXTENDS Widget
    FIELD PreviewPane.app,         8
    FIELD PreviewPane.buf,         8
    FIELD PreviewPane.buf_len,     8
    FIELD PreviewPane.file_size,   8
    FIELD PreviewPane.scroll,      4
    FIELD PreviewPane.total_lines, 4
    FIELD PreviewPane.is_binary,   4
    FIELD PreviewPane.is_dir,      4
    FIELD PreviewPane.file_mode,   4
    FIELD PreviewPane.pending,     4   # 1 while file head is being read asynchronously
    FIELD PreviewPane.thumb_len,   4   # image payload bytes at thumb_ptr (0 = not an image)
    FIELD PreviewPane.view_mode,   4   # PV_MODE_* chosen with Alt+V (reset per file)
    FIELD PreviewPane.thumb_ptr,   8   # IMG_THUMB_HDR + payload (helper shared buffer or buf)
    FIELD PreviewPane.thumb_mode,  4   # IMG_PROTO_* the payload was produced for
    FIELD PreviewPane.thumb_gen,   4   # bumped per accepted payload (overlay change detection)
    FIELD PreviewPane.req_mode,    4   # IMG_PROTO_* | cols << 8 of the pending request
    FIELD PreviewPane.target_path, PATH_MAX
CLASS_END PreviewPane

CLASS_BEGIN Dialog
    EXTENDS Widget
    FIELD Dialog.app,         8
    FIELD Dialog.title,       8
    FIELD Dialog.result,      4
    FIELD Dialog.focus_btn,   4
    FIELD Dialog.hovered_btn, 4
    FIELD Dialog.btn_count,   4
    FIELD Dialog.op_tag,      4
    FIELD Dialog.pad,         4
    FIELD Dialog.btns,        4*HitZone._size
CLASS_END Dialog

CLASS_BEGIN ConfirmDialog
    EXTENDS Dialog
    FIELD ConfirmDialog.message, 512
    FIELD ConfirmDialog.detail,  512
CLASS_END ConfirmDialog

CLASS_BEGIN InputDialog
    EXTENDS Dialog
    FIELD InputDialog.prompt, 128
    FIELD InputDialog.buf,    INPUT_MAX
    FIELD InputDialog.len,    4
    FIELD InputDialog.cursor, 4
    FIELD InputDialog.scroll, 4
    FIELD InputDialog.pad,    4
CLASS_END InputDialog

CLASS_BEGIN ViewerDialog
    EXTENDS Dialog
    FIELD ViewerDialog.data,        8   # mmap'ed file (map_len != 0) or heap fallback buffer
    FIELD ViewerDialog.data_len,    8
    FIELD ViewerDialog.map_len,     8   # nonzero when data is a read-only file mapping
    FIELD ViewerDialog.top_off,     8   # text mode: byte offset of the top line
    FIELD ViewerDialog.top_line,    8   # text mode: 0-based index of the top line
    FIELD ViewerDialog.total_lines, 8   # lazily counted (SIMD) on End; -1 = unknown
    FIELD ViewerDialog.hex_row,     8   # hex mode: top 16-byte row
    FIELD ViewerDialog.hex_mode,    4
    FIELD ViewerDialog.vis_rows,    4   # rows rendered last frame (page size)
    FIELD ViewerDialog.hex_edit_ctx, 8  # HexEditCtx* when editing in hex mode
    FIELD ViewerDialog.img_mode,    4   # 1 = picture view (Image decoded from data)
    FIELD ViewerDialog.img_state,   4   # 0 = not decoded yet, 1 = ok, < 0 = IMG_E_*
    FIELD ViewerDialog.img,         Image._size
    FIELD ViewerDialog.thumb,       8   # img scaled to the window (RGBA), rebuilt on resize
    FIELD ViewerDialog.thumb_w,     4
    FIELD ViewerDialog.thumb_h,     4
    FIELD ViewerDialog.enc,         8   # Sixel / Kitty stream of thumb (pixel protocols)
    FIELD ViewerDialog.enc_len,     8
    FIELD ViewerDialog.enc_key,     8   # protocol | size the stream was built for
    FIELD ViewerDialog.img_zoom,    4   # 0 = Fit to window; > 0 = percent (25..800)
    FIELD ViewerDialog.img_braille, 4   # 1 = 2x4 Braille high-detail mode
    FIELD ViewerDialog.img_pan_x,   4   # top-left source pixel X (clamped)
    FIELD ViewerDialog.img_pan_y,   4   # top-left source pixel Y (clamped)
    FIELD ViewerDialog.img_box_w,   4   # last viewport width in output pixels
    FIELD ViewerDialog.img_box_h,   4   # last viewport height in output pixels
    FIELD ViewerDialog.img_sb_drag, 4   # active scrollbar drag: 0=none, 1=V, 2=H
    FIELD ViewerDialog.img_cache_key, 8 # zoom/pan key of current thumb
    FIELD ViewerDialog.path,        PATH_MAX
CLASS_END ViewerDialog

CLASS_BEGIN ViewAsDialog
    EXTENDS Dialog
    FIELD ViewAsDialog.choice,      4   # PV_MODE_IMAGE / HEX / TEXT
    FIELD ViewAsDialog.fmt,         4   # IMG_FMT_* of the file
    FIELD ViewAsDialog.path,        PATH_MAX
CLASS_END ViewAsDialog

CLASS_BEGIN HelpDialog
    EXTENDS Dialog
    FIELD HelpDialog.scroll,    4
    FIELD HelpDialog.tab,       4       # 0 = Overview, 1 = Instruction
    FIELD HelpDialog.tab_zones, 2*HitZone._size
CLASS_END HelpDialog

CLASS_BEGIN ProgressDialog
    EXTENDS Dialog
    FIELD ProgressDialog.btn_lbl, 3*8   # current labels of [Pause|Resume, Background, Cancel]
CLASS_END ProgressDialog

CLASS_BEGIN UserMenuDialog
    EXTENDS Dialog
    FIELD UserMenuDialog.cursor, 4
    FIELD UserMenuDialog.scroll, 4
CLASS_END UserMenuDialog

CLASS_BEGIN BookmarkDialog
    EXTENDS Dialog
    FIELD BookmarkDialog.mode,       4   # 0=Bookmarks, 1=Frecency
    FIELD BookmarkDialog.cursor,     4
    FIELD BookmarkDialog.scroll,     4
    FIELD BookmarkDialog.filter_len, 4
    FIELD BookmarkDialog.match_cnt,  4
    FIELD BookmarkDialog.pad,        4
    FIELD BookmarkDialog.filter,     64
    FIELD BookmarkDialog.matches,    128*4
CLASS_END BookmarkDialog

.include "dialogs_ext.inc"

CLASS_BEGIN App
    EXTENDS Object
    FIELD App.screen,        8
    FIELD App.theme,         8
    FIELD App.lang,          8
    FIELD App.left_pane,     8
    FIELD App.right_pane,    8
    FIELD App.active_pane,   8
    FIELD App.preview_pane,  8
    FIELD App.header_bar,    8
    FIELD App.status_bar,    8
    FIELD App.func_bar,      8
    FIELD App.active_dialog, 8
    FIELD App.envp,          8
    FIELD App.running,       4
    FIELD App.need_redraw,   4
    FIELD App.theme_idx,     4
    FIELD App.lang_idx,      4
    FIELD App.right_mode,    4
    FIELD App.term_w,        4
    FIELD App.term_h,        4
    FIELD App.mouse_x,       4
    FIELD App.mouse_y,       4
    FIELD App.stream_turn,   4          # alternates streaming steps between panes
    FIELD App.task_dlg_at,   8          # monotonic ms when ProgressDialog may auto-open
    FIELD App.task_last_ms,  8          # last snapshot refresh
    FIELD App.task_active,   4          # 1 while a background worker runs
    FIELD App.task_hidden,   4          # 1 after user sent the dialog to background
    FIELD App.preview_due,   8          # debounced preview deadline (0 = none)
    FIELD App.watch_due,     16         # per pane: debounced auto-refresh deadline (0 = none)
    FIELD App.watch_last,    16         # per pane: last auto-refresh (rate limit)
    FIELD App.watch_kind,    8          # per pane: 1 = structural, 2 = content
    FIELD App.preview_seq,   4          # sequence of the preview request we wait for
    FIELD App.ev_pad,        4
    FIELD App.task_snap,     TaskSnap._size
CLASS_END App

# --- Operation Tags for Dialogs ---
.equ OP_NONE,    0
.equ OP_MKDIR,   1
.equ OP_RENAME,  2
.equ OP_COPY,    3
.equ OP_MOVE,    4
.equ OP_DELETE,  5
.equ OP_NEWFILE, 6
.equ OP_FILTER,  7
.equ OP_GOTO,    8
.equ OP_CHMOD,   9
.equ OP_PROGRESS, 10
.equ OP_QUIT,     11
.equ OP_USERMENU,  12
.equ OP_BOOKMARKS, 13
.equ OP_FRECENCY,  14
.equ OP_CONFLICT,  15
.equ OP_SEARCH,    16
.equ OP_INFO,      17
.equ OP_SYMLINK,   18
.equ OP_HARDLINK,  19
.equ OP_PALETTE,   20
.equ OP_FUZZY,     21
.equ OP_VIEWAS,    22
.equ OP_UNPACK,    23

# --- HeaderBar Action IDs ---
.equ HDR_BTN_THEME,   1
.equ HDR_BTN_PREVIEW, 2
.equ HDR_BTN_HIDDEN,  3
.equ HDR_BTN_HELP,    4
.equ HDR_BTN_QUIT,    5
.equ HDR_BTN_LANG,    6

# --- Status Message Kinds ---
.equ MSG_INFO, 0
.equ MSG_OK,   1
.equ MSG_WARN, 2
.equ MSG_ERR,  3
