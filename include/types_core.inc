# Core Data Structure Layouts (Value Types & Palette); LangStrings (the i18n slots) is in types.inc.
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
TaskShared.skipped:     .space 8   # members the worker could not process but went on (unpack / test): unsafe, special, encrypted, unsupported, damaged
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
