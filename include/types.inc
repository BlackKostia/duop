# Core Data Structure Layouts (Value Types & Palette)
.intel_syntax noprefix
.struct 0
Rect.x:        .space 4
Rect.y:        .space 4
Rect.w:        .space 4
Rect.h:        .space 4
Rect._size:
.struct 0
Cell.ch:       .space 4    # Unicode codepoint (or CELL_WIDE_TAIL)
Cell.fg:       .space 4    # 0x01RRGGBB (0 = default)
Cell.bg:       .space 4    # 0x01RRGGBB (0 = default)
Cell.attrs:    .space 4    # ATTR_* bitmask
Cell._size:
# --- Image (24 bytes): decoded picture, RGBA8 rows (src/fs/img_*.s) ---
.struct 0
Image.px:      .space 8    # w * h * 4 bytes (mem_alloc), R,G,B,A per pixel
Image.w:       .space 4
Image.h:       .space 4
Image.fmt:     .space 4    # IMG_FMT_*
Image.pad:     .space 4
Image._size:
# --- ImgPlace (48 bytes): a pixel-protocol image placed over a cell rectangle ---
.struct 0
ImgPlace.owner:  .space 8    # widget / dialog that shows it (hidden while another dialog is up)
ImgPlace.data:   .space 8    # ready Sixel / Kitty escape stream
ImgPlace.len:    .space 8
ImgPlace.key:    .space 8    # changes whenever the picture changes
ImgPlace.x:      .space 4    # cell rectangle covered by the picture
ImgPlace.y:      .space 4
ImgPlace.w:      .space 4
ImgPlace.h:      .space 4
ImgPlace._size:
# --- Event (32 bytes) ---
.struct 0
Event.type:       .space 4   # EVT_*
Event.mods:       .space 4   # MOD_*
Event.key:        .space 4   # Unicode codepoint or KEY_*
Event.mouse_x:    .space 4   # 0-based col
Event.mouse_y:    .space 4   # 0-based row
Event.mouse_btn:  .space 4   # MOUSE_*
Event.mouse_kind: .space 4   # MACT_*
Event.lat:        .space 4   # layout-independent hotkey: Latin key under `key` (*.layout files), else = key
Event._size:
# --- HitZone (8 bytes) for clickable/hoverable regions ---
.struct 0
HitZone.x1:    .space 2    # inclusive start col
HitZone.x2:    .space 2    # exclusive end col
HitZone.y:     .space 2    # row
HitZone.id:    .space 2    # action / button ID
HitZone._size:
# --- FileEntry (32 bytes, compact metadata + name_off into DirModel.name_pool) ---
.struct 0
FileEntry.size:       .space 8    # st_size (i64)
FileEntry.mtime:      .space 8    # st_mtime seconds (i64)
FileEntry.mode:       .space 4    # st_mode (u32)
FileEntry.flags:      .space 4    # FE_* bitmask
FileEntry.name_off:   .space 4    # byte offset inside DirModel.name_pool
FileEntry.name_len:   .space 2    # bytes in name (without NUL)
FileEntry.disp_width: .space 1    # terminal columns of name (clamped 0..255)
FileEntry.ext_offset: .space 1    # offset of '.' after index 0, or 0
FileEntry._size:
# --- TaskShared (MAP_SHARED page written by the background worker, read by the UI) ---
# Counters are naturally aligned qwords (atomic on all targets); cur_name uses a seqlock.
.struct 0
TaskShared.total_bytes: .space 8   # bytes of regular files discovered by the scan phase
TaskShared.total_files: .space 8   # entries (files + dirs + links) discovered by the scan phase
TaskShared.done_bytes:  .space 8   # bytes copied so far
TaskShared.done_files:  .space 8   # entries processed so far
TaskShared.result:      .space 8   # final 0 / -errno (valid once phase == TASK_PHASE_DONE)
TaskShared.seq:         .space 4   # seqlock counter for cur_name (odd = writer active)
TaskShared.phase:       .space 4   # TASK_PHASE_*
TaskShared.cancel:      .space 4   # set to 1 by the UI process to request cooperative cancel
TaskShared.conflict_state:  .space 4   # 0=none, 1=worker waiting, 2=UI decision ready
TaskShared.conflict_action: .space 4   # CONFLICT_ACT_*
TaskShared.conflict_all:    .space 4   # 1=apply to all remaining conflicts
TaskShared.conflict_src_sz: .space 8
TaskShared.conflict_dst_sz: .space 8
TaskShared.cur_name:    .space 256      # == NAME_MAX (asserted in src/fs/task.s)
TaskShared.conflict_name: .space 256    # == NAME_MAX
TaskShared._size:

# --- TaskSnap (UI-side consistent snapshot of the running task) ---
.struct 0
TaskSnap.total_bytes: .space 8
TaskSnap.total_files: .space 8
TaskSnap.done_bytes:  .space 8
TaskSnap.done_files:  .space 8
TaskSnap.elapsed_ms:  .space 8     # active time (pauses excluded)
TaskSnap.speed_bps:   .space 8     # bytes per second (0 = unknown)
TaskSnap.eta_sec:     .space 8     # seconds remaining (-1 = unknown)
TaskSnap.op:          .space 4     # TASK_OP_*
TaskSnap.phase:       .space 4     # TASK_PHASE_*
TaskSnap.paused:      .space 4
TaskSnap.percent:     .space 4     # 0..100
TaskSnap.canceling:   .space 4
TaskSnap.pad:         .space 4
TaskSnap.cur_name:    .space 256        # == NAME_MAX
TaskSnap._size:

# --- TaskJob (heap job description handed to fs_task_start; names are NUL-separated) ---
.struct 0
TaskJob.names_len: .space 8        # bytes used in names[]
TaskJob.op:        .space 4        # TASK_OP_*
TaskJob.count:     .space 4        # number of names
TaskJob.join_dst:  .space 4        # 1 = dst/<name>, 0 = dst used verbatim (single item)
TaskJob.pad:       .space 4
TaskJob.src_dir:   .space 4096         # == PATH_MAX (asserted in src/fs/task.s)
TaskJob.dst:       .space 4096
TaskJob.names:                     # variable-length "a\0b\0..." block follows
TaskJob._size:

# --- DCSlot (LRU directory cache snapshot, see src/fs/dir_cache.s) ---
.struct 0
DCSlot.entries:     .space 8
DCSlot.order:       .space 8
DCSlot.name_pool:   .space 8
DCSlot.raw_count:   .space 8
DCSlot.raw_cap:     .space 8
DCSlot.name_used:   .space 8
DCSlot.name_cap:    .space 8
DCSlot.count:       .space 8
DCSlot.dev:         .space 8
DCSlot.ino:         .space 8
DCSlot.mtime:       .space 8
DCSlot.lru:         .space 8
DCSlot.bytes:       .space 8
DCSlot.mtime_ns:    .space 4
DCSlot.used:        .space 4
DCSlot.dir_count:   .space 4
DCSlot.file_count:  .space 4
DCSlot.has_dotdot:  .space 4
DCSlot.sort_mode:   .space 4
DCSlot.sort_desc:   .space 4
DCSlot.show_hidden: .space 4
DCSlot.is_unsorted: .space 4
DCSlot.huge_mode:   .space 4
DCSlot.filter:      .space 64      # == FILTER_MAX
DCSlot.path:        .space 4096    # == PATH_MAX
DCSlot._size:

# --- Theme (128 bytes) ---
.struct 0
Theme.name:               .space 8
Theme.bg:                 .space 4
Theme.surface:            .space 4
Theme.surface_alt:        .space 4
Theme.header_bg:          .space 4
Theme.header_fg:          .space 4
Theme.accent:             .space 4
Theme.accent_fg:          .space 4
Theme.border_active:      .space 4
Theme.border_inactive:    .space 4
Theme.text:               .space 4
Theme.muted:              .space 4
Theme.dir_fg:             .space 4
Theme.exec_fg:            .space 4
Theme.link_fg:            .space 4
Theme.hidden_fg:          .space 4
Theme.selected_fg:        .space 4
Theme.selected_bg:        .space 4
Theme.cursor_active_bg:   .space 4
Theme.cursor_inactive_bg: .space 4
Theme.badge_bg:           .space 4
Theme.badge_hover_bg:     .space 4
Theme.badge_fg:           .space 4
Theme.dialog_bg:          .space 4
Theme.dialog_border:      .space 4
Theme.ok_fg:              .space 4
Theme.warn_fg:            .space 4
Theme.err_fg:             .space 4
Theme.scroll_track:       .space 4
Theme.scroll_thumb:       .space 4
Theme.pad:                .space 4
Theme._size:

# --- DuProgress: live counters of a Disk Usage subtree walk (fs/du_parallel.s) ---
.struct 0
DuProgress.start_ms: .space 8   # os_clock_ms() when the walk began
DuProgress.dirs:     .space 8   # directories read so far (all depths)
DuProgress.files:    .space 8   # regular files counted so far
DuProgress.bytes:    .space 8   # their bytes
DuProgress.done:     .space 4   # top-level folders finished
DuProgress.total:    .space 4   # top-level folders to walk
DuProgress.cancel:   .space 4   # set by the progress callback (Esc): walkers stop
DuProgress.phase:    .space 4   # DU_PHASE_RUN, then DU_PHASE_DONE for the final call
DuProgress._size:

# --- DataSpec: one kind of runtime data file (tui/data_dirs.s) ---
.struct 0
DataSpec.env:   .space 8   # "DUOP_LAYOUTS=": ':'-separated dirs that replace the default path
DataSpec.sub:   .space 8   # "layouts": subdirectory under duop/ config + share dirs and the binary
DataSpec.ext:   .space 8   # ".layout": file name suffix that is loaded
DataSpec.load:  .space 8   # void load(const char *path, const char *name)
DataSpec._size:
# --- LangStrings (Table-driven i18n string bundle) ---
.struct 0
LangStrings.code:               .space 8
LangStrings.name:               .space 8
LangStrings.hdr_brand:          .space 8
LangStrings.hdr_mode_dual:      .space 8
LangStrings.hdr_mode_prev:      .space 8
LangStrings.hdr_btn_quit:       .space 8
LangStrings.hdr_btn_help:       .space 8
LangStrings.hdr_btn_lang:       .space 8
LangStrings.hdr_btn_hid_on:     .space 8
LangStrings.hdr_btn_hid_off:    .space 8
LangStrings.hdr_btn_prev:       .space 8
LangStrings.hdr_theme_pfx:      .space 8
LangStrings.f_labels:           .space 10*8
LangStrings.sb_dir_tag:         .space 8
LangStrings.sb_filter_pfx:      .space 8
LangStrings.sb_filter_sfx:      .space 8
LangStrings.sb_hint_right:      .space 8
LangStrings.msg_lang_switched:  .space 8
LangStrings.fp_col_name:        .space 8
LangStrings.fp_col_size:        .space 8
LangStrings.fp_col_mod:         .space 8
LangStrings.fp_col_perm:        .space 8
LangStrings.fp_dir_badge:       .space 8
LangStrings.fp_sel_pfx:         .space 8
LangStrings.fp_sel_mid:         .space 8
LangStrings.fp_sel_end:         .space 8
LangStrings.fp_sum_dirs:        .space 8
LangStrings.fp_sum_files:       .space 8
LangStrings.fp_flt_tag:         .space 8
LangStrings.pv_title:           .space 8
LangStrings.pv_dir_msg:         .space 8
LangStrings.pv_bin_hdr:         .space 8
LangStrings.pv_empty:           .space 8
LangStrings.pv_sz_lbl:          .space 8
LangStrings.pv_perm_lbl:        .space 8
LangStrings.err_perm:           .space 8
LangStrings.err_exists:         .space 8
LangStrings.err_noent:          .space 8
LangStrings.err_notempty:       .space 8
LangStrings.err_isdir:          .space 8
LangStrings.err_notdir:         .space 8
LangStrings.err_same:           .space 8
LangStrings.err_generic:        .space 8
LangStrings.dlg_btn_ok:         .space 8
LangStrings.dlg_btn_cancel:     .space 8
LangStrings.dlg_btn_yes:        .space 8
LangStrings.dlg_btn_no:         .space 8
LangStrings.dlg_btn_close:      .space 8
LangStrings.dlg_btn_hex:        .space 8
LangStrings.dlg_title_help:     .space 8
LangStrings.dlg_title_viewer:   .space 8
LangStrings.dlg_vw_hint:        .space 8
LangStrings.dlg_hlp_hint:       .space 8
LangStrings.help_lines:         .space 22*8
LangStrings.app_title_mkdir:    .space 8
LangStrings.app_prm_mkdir:      .space 8
LangStrings.app_title_newfile:  .space 8
LangStrings.app_prm_newfile:    .space 8
LangStrings.app_title_rename:   .space 8
LangStrings.app_prm_rename:     .space 8
LangStrings.app_title_copy:     .space 8
LangStrings.app_prm_copy:       .space 8
LangStrings.app_title_move:     .space 8
LangStrings.app_prm_move:       .space 8
LangStrings.app_title_filter:   .space 8
LangStrings.app_prm_filter:     .space 8
LangStrings.app_title_goto:     .space 8
LangStrings.app_prm_goto:       .space 8
LangStrings.app_title_delete:   .space 8
LangStrings.app_msg_del_one:    .space 8
LangStrings.app_msg_del_many:   .space 8
LangStrings.app_sel_items_sfx:  .space 8
LangStrings.app_ok_mkdir:       .space 8
LangStrings.app_ok_newfile:     .space 8
LangStrings.app_ok_rename:      .space 8
LangStrings.app_ok_copy:        .space 8
LangStrings.app_ok_move:        .space 8
LangStrings.app_ok_delete:      .space 8
LangStrings.app_ok_refresh:     .space 8
LangStrings.app_err_parent:     .space 8
LangStrings.app_err_isdir:      .space 8
LangStrings.fp_loading_pfx:     .space 8
LangStrings.fp_huge_badge:      .space 8
LangStrings.fp_abort_badge:     .space 8
LangStrings.app_load_aborted:   .space 8
LangStrings.task_title_copy:    .space 8
LangStrings.task_title_move:    .space 8
LangStrings.task_title_delete:  .space 8
LangStrings.task_btn_pause:     .space 8
LangStrings.task_btn_resume:    .space 8
LangStrings.task_btn_bg:        .space 8
LangStrings.task_lbl_files:     .space 8
LangStrings.task_lbl_eta:       .space 8
LangStrings.task_lbl_scan:      .space 8
LangStrings.task_lbl_paused:    .space 8
LangStrings.task_lbl_canceling: .space 8
LangStrings.task_sb_hint:       .space 8
LangStrings.err_canceled:       .space 8
LangStrings.err_busy:           .space 8
LangStrings.app_title_quit_task: .space 8
LangStrings.app_msg_quit_task:  .space 8
LangStrings.dlg_title_usermenu: .space 8
LangStrings.um_hint:            .space 8
LangStrings.dlg_title_bookmarks: .space 8
LangStrings.bm_hint:            .space 8
LangStrings.dlg_title_frecency: .space 8
LangStrings.fre_hint:           .space 8
LangStrings.msg_bm_added:       .space 8
LangStrings.msg_hist_empty:     .space 8
LangStrings.msg_tab_last:       .space 8
LangStrings.dlg_title_conflict: .space 8
LangStrings.cfl_btn_overwrite:  .space 8
LangStrings.cfl_btn_skip:       .space 8
LangStrings.cfl_btn_rename:     .space 8
LangStrings.cfl_lbl_forall:     .space 8
LangStrings.cfl_hint:           .space 8
LangStrings.dlg_title_search:   .space 8
LangStrings.srch_lbl_name:      .space 8
LangStrings.srch_lbl_grep:      .space 8
LangStrings.srch_hint:          .space 8
LangStrings.fp_search_badge:    .space 8
LangStrings.msg_search_none:    .space 8
LangStrings.dlg_title_info:     .space 8
LangStrings.dlg_title_chmod:    .space 8
LangStrings.dlg_title_symlink:  .space 8
LangStrings.dlg_title_hardlink: .space 8
LangStrings.prm_link_target:    .space 8
LangStrings.chm_hint:           .space 8
LangStrings.msg_trashed:        .space 8
LangStrings.msg_undone:         .space 8
LangStrings.msg_undo_empty:     .space 8
LangStrings.dlg_title_palette:  .space 8
LangStrings.dlg_title_fuzzy:    .space 8
LangStrings.fuz_hint:           .space 8
LangStrings.fp_du_badge:        .space 8
LangStrings.br_title:           .space 8
LangStrings.br_tmpl:            .space 8
LangStrings.br_find:            .space 8
LangStrings.br_repl:            .space 8
LangStrings.ctx_view:           .space 8
LangStrings.ctx_copy:           .space 8
LangStrings.ctx_move:           .space 8
LangStrings.ctx_copy_path:      .space 8
LangStrings.ctx_delete:         .space 8
LangStrings.msg_copied_path:    .space 8
LangStrings.diff_pfx:           .space 8
LangStrings.term_title:         .space 8
LangStrings.img_unsup:          .space 8
LangStrings.dlg_title_viewas:   .space 8
LangStrings.vas_image:          .space 8
LangStrings.vas_hex:            .space 8
LangStrings.vas_text:           .space 8
LangStrings.vas_hint:           .space 8
LangStrings.vw_img_hint:        .space 8
LangStrings.img_fit:            .space 8
LangStrings.pal_theme:          .space 8   # Command Palette (Ctrl+K) entry
LangStrings.pal_lang:           .space 8
LangStrings.pal_preview:        .space 8
LangStrings.pal_hidden:         .space 8
LangStrings.pal_help:           .space 8
LangStrings.pal_rename:         .space 8
LangStrings.pal_view:           .space 8
LangStrings.pal_edit:           .space 8
LangStrings.pal_copy:           .space 8
LangStrings.pal_move:           .space 8
LangStrings.pal_mkdir:          .space 8
LangStrings.pal_delete:         .space 8
LangStrings.pal_newfile:        .space 8
LangStrings.pal_filter:         .space 8
LangStrings.pal_goto:           .space 8
LangStrings.pal_refresh:        .space 8
LangStrings.pal_usermenu:       .space 8
LangStrings.pal_shell:          .space 8
LangStrings.pal_quit:           .space 8
LangStrings.cli_version:        .space 8   # duop --version text (multi-line)
LangStrings.cli_usage:          .space 8   # duop --help text (multi-line)
LangStrings.help_tab_overview:  .space 8   # HelpDialog tab 0 label
LangStrings.help_tab_instr:     .space 8   # HelpDialog tab 1 label
LangStrings.help_instr_lines:   .space 36*8
LangStrings.du_scan_title:      .space 8   # Disk Usage progress window
LangStrings.du_lbl_folders:     .space 8
LangStrings.du_lbl_scanned:     .space 8
LangStrings.du_unit_dirs:       .space 8
LangStrings.du_unit_files:      .space 8
LangStrings.du_scan_hint:       .space 8
LangStrings.msg_du_canceled:    .space 8
LangStrings.pkg_title:          .space 8
LangStrings.pkg_hint:           .space 8
LangStrings.pkg_empty:          .space 8
LangStrings.pkg_install_prompt: .space 8
LangStrings.pkg_installed:      .space 8
LangStrings.pkg_removed:        .space 8
LangStrings.pkg_verified:       .space 8
LangStrings.pkg_err:            .space 8
LangStrings.pkg_tab_inst:       .space 8
LangStrings.pkg_tab_avail:      .space 8
LangStrings.arc_truncated:      .space 8
LangStrings.arc_readonly:       .space 8
LangStrings.err_cmd_long:       .space 8
LangStrings.cfg_truncated:      .space 8
LangStrings.err_special:        .space 8
LangStrings._size:
.section .text
