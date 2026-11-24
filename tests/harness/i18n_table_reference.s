# Frozen reference oracle: the hand-written asm that build/i18n_en.c (tools/gen_i18n.py from src/tui/i18n_en.lang) replaced (tests/test_c_i18n_table.py diffs against it)
# Internationalization (i18n): the built-in English string table -- the base and fallback of every
# interface language. Other languages are *.lang packs loaded at runtime (i18n_pack.s, lang/uk.lang).
.intel_syntax noprefix
.include "classes.inc"
.include "version.inc"
.section .rodata
# ==============================================================================
# English String Table (index 0, id "en")
# ==============================================================================
en_code:            .asciz "EN"
en_name:            .asciz "English"
en_hdr_brand:       .asciz " ◆ Duop "
en_hdr_mode_dual:   .asciz " Dual "
en_hdr_mode_prev:   .asciz " Preview "
en_hdr_btn_quit:    .asciz " Quit "
en_hdr_btn_help:    .asciz " Help "
en_hdr_btn_lang:    .asciz " Lang:EN "
en_hdr_btn_hid_on:  .asciz " Dot:ON "
en_hdr_btn_hid_off: .asciz " Dot:OFF "
en_hdr_btn_prev:    .asciz " Preview "
en_hdr_theme_pfx:   .asciz " Theme:"

en_flbl_1:          .asciz "Help"
en_flbl_2:          .asciz "Ren"
en_flbl_3:          .asciz "View"
en_flbl_4:          .asciz "Edit"
en_flbl_5:          .asciz "Copy"
en_flbl_6:          .asciz "Move"
en_flbl_7:          .asciz "Mkdir"
en_flbl_8:          .asciz "Del"
en_flbl_9:          .asciz "Prev"
en_flbl_0:          .asciz "Quit"

en_sb_dir_tag:      .asciz "<DIR>"
en_sb_filter_pfx:   .asciz " Filter: \""
en_sb_filter_sfx:   .asciz "\"  [Esc clear]"
en_sb_hint_right:   .asciz "Tab:Switch  Space:Mark  /:Filter  Alt+T/L"
en_msg_lang_sw:     .asciz "Interface language: English"

en_fp_col_name:     .asciz "Name"
en_fp_col_size:     .asciz "Size"
en_fp_col_mod:      .asciz "Modified"
en_fp_col_perm:     .asciz "Perms"
en_fp_dir_badge:    .asciz "<DIR>"
en_fp_sel_pfx:      .asciz " ✓ "
en_fp_sel_mid:      .asciz " selected ("
en_fp_sel_end:      .asciz ")"
en_fp_sum_dirs:     .asciz " dirs, "
en_fp_sum_files:    .asciz " files • "
en_fp_flt_tag:      .asciz " filter:"

en_pv_title:        .asciz " ◈ Live Preview "
en_pv_dir_msg:      .asciz "Directory — Press Enter or double-click to browse"
en_pv_bin_hdr:      .asciz "Binary file detected — Hex preview:"
en_pv_empty:        .asciz "(Empty file)"
en_pv_sz_lbl:       .asciz "Size: "
en_pv_perm_lbl:     .asciz "  Perms: "

en_err_perm:        .asciz "Permission denied"
en_err_exists:      .asciz "Destination already exists"
en_err_noent:       .asciz "File or directory not found"
en_err_notempty:    .asciz "Directory not empty"
en_err_isdir:       .asciz "Target is a directory"
en_err_notdir:      .asciz "Not a directory"
en_err_same:        .asciz "Source and destination are the same"
en_err_generic:     .asciz "Filesystem operation failed"

en_dlg_btn_ok:      .asciz "  OK  "
en_dlg_btn_cancel:  .asciz " Cancel "
en_dlg_btn_yes:     .asciz "  Yes  "
en_dlg_btn_no:      .asciz "  No   "
en_dlg_btn_close:   .asciz " Close "
en_dlg_btn_hex:     .asciz " Hex/Text "
en_dlg_title_help:
    .ascii "Duop Help & Architecture (v"
    DUOP_VER_ASCII
    .asciz ")"
en_dlg_title_view:  .asciz "File Viewer"
en_dlg_vw_hint:     .asciz "↑↓/Wheel: Scroll   H: Text/Hex   Esc/Q: Close"
en_dlg_hlp_hint:    .asciz "↑↓/Wheel: Scroll   Esc/Enter: Close"

en_hl_0:  .asciz "◆ KEYBOARD NAVIGATION"
en_hl_1:  .asciz "  ↑/↓, j/k, PgUp/PgDn, Home/End  Move cursor in active panel"
en_hl_2:  .asciz "  Tab                            Switch between Left and Right panels"
en_hl_3:  .asciz "  Enter / Backspace / ~          Open item / Parent dir / Home dir"
en_hl_4:  .asciz "  Space / Insert / * / Ctrl+A    Mark file / Toggle mark all"
en_hl_5:  .asciz "  /                              Live filename filter (Esc clears)"
en_hl_6:  .asciz "  . or Ctrl+H                    Toggle hidden (dot) files"
en_hl_7:  .asciz "  Alt+T / Alt+L / Ctrl+P / G     Theme / Language / Preview / Go to path"
en_hl_8:  .asciz "◆ FILE OPERATIONS (F-KEYS & SHORTCUTS)"
en_hl_9:  .asciz "  F2 Rename   F3 View (Image/Hex/Text)   F4 Edit   Ctrl+N New File"
en_hl_10: .asciz "  F5 Copy     F6 Move              F7 Mkdir            F8/Del Delete"
en_hl_11: .asciz "  F9 Preview (Alt+V Image/Hex/Text)  Ctrl+O Shell  Ctrl+R  F10 Quit"
en_hl_12: .asciz "◆ MOUSE SUPPORT (SGR 1006)"
en_hl_13: .asciz "  Left Click / Double Click        Select row / Open dir or View file"
en_hl_14: .asciz "  Right Click / Scroll Wheel       Mark file / Scroll panel under mouse"
en_hl_15: .asciz "  Click Column Header / Scrollbar  Sort Name/Size/Date / Drag scrollbar"
en_hl_16: .asciz "◆ ARCHITECTURE (Pure x86-64 GAS + Linux Syscalls)"
en_hl_17: .asciz "  F5/F6/F8 run in a fork worker: Alt+J progress, P pause, Esc hide, C cancel"
en_hl_18: .asciz "◆ AUTHOR & OPEN-SOURCE LICENSE"
en_hl_19: .asciz "  Developer: Kostiantyn Nedbailo (Недбайло Костянтин Геннадійович)"
en_hl_20: .asciz "  Email:     ChessPlayerSedoy@gmail.com"
en_hl_21: .asciz "  License:   MIT License (OSI Open Source — see LICENSE)"

en_app_title_mkdir:   .asciz "Create Directory"
en_app_prm_mkdir:     .asciz "Directory name:"
en_app_title_newfile: .asciz "Create New File"
en_app_prm_newfile:   .asciz "File name:"
en_app_title_rename:  .asciz "Rename"
en_app_prm_rename:    .asciz "New name:"
en_app_title_copy:    .asciz "Copy"
en_app_prm_copy:      .asciz "Copy destination path:"
en_app_title_move:    .asciz "Move"
en_app_prm_move:      .asciz "Move destination path:"
en_app_title_filter:  .asciz "Live Filter"
en_app_prm_filter:    .asciz "Filter substring (empty clears):"
en_app_title_goto:    .asciz "Go To Directory"
en_app_prm_goto:      .asciz "Target directory path:"
en_app_title_delete:  .asciz "Confirm Delete"
en_app_msg_del_one:   .asciz "Delete this item permanently?"
en_app_msg_del_many:  .asciz "Delete selected items permanently?"
en_app_sel_items_sfx: .asciz " item(s) selected"
en_app_ok_mkdir:      .asciz "Directory created"
en_app_ok_newfile:    .asciz "File created"
en_app_ok_rename:     .asciz "Renamed successfully"
en_app_ok_copy:       .asciz "Copied successfully"
en_app_ok_move:       .asciz "Moved successfully"
en_app_ok_delete:     .asciz "Deleted successfully"
en_app_ok_refresh:    .asciz "Refreshed"
en_app_err_parent:    .asciz "Cannot operate on '..'"
en_app_err_isdir:     .asciz "Cannot view/edit a directory"
en_fp_loading_pfx:    .asciz " Loading: "
en_fp_huge_badge:     .asciz " [FS-order]"
en_fp_abort_badge:    .asciz " [Esc:stopped]"
en_app_load_aborted:  .asciz "Directory streaming stopped by Esc"
en_task_title_copy:   .asciz "Copying"
en_task_title_move:   .asciz "Moving"
en_task_title_delete: .asciz "Deleting"
en_task_btn_pause:    .asciz " Pause "
en_task_btn_resume:   .asciz " Resume "
en_task_btn_bg:       .asciz " Background "
en_task_lbl_files:    .asciz "Files: "
en_task_lbl_eta:      .asciz "ETA "
en_task_lbl_scan:     .asciz "Scanning… "
en_task_lbl_paused:   .asciz "Paused"
en_task_lbl_cancel:   .asciz "Canceling…"
en_task_sb_hint:      .asciz "Alt+J: details"
en_err_canceled:      .asciz "Operation canceled"
en_err_busy:          .asciz "Another background operation is running"
en_app_title_quit_tk: .asciz "Background Operation"
en_app_msg_quit_tk:   .asciz "An operation is still running. Cancel it and quit?"
en_dlg_title_usermenu: .asciz "User Menu"
en_um_hint:           .asciz "Enter: Run  •  Esc: Close"
en_dlg_title_bm:      .asciz "Bookmarks (Hotlist)"
en_bm_hint:           .asciz "Enter: Jump  •  a: Add  •  d: Remove  •  Esc: Close"
en_dlg_title_fre:     .asciz "Frecency Jump (z)"
en_fre_hint:          .asciz "Type substring • Up/Down • Enter: Jump • Esc: Close"
en_msg_bm_added:      .asciz "Bookmark added"
en_msg_hist_empty:    .asciz "No history"
en_msg_tab_last:      .asciz "Cannot close last tab"
en_dlg_title_conflict: .asciz "File Conflict"
en_cfl_btn_overwrite: .asciz "Overwrite"
en_cfl_btn_skip:      .asciz "Skip"
en_cfl_btn_rename:    .asciz "Rename"
en_cfl_lbl_forall:    .asciz "For all"
en_cfl_hint:          .asciz "o: Overwrite • s: Skip • r: Rename • a: For all • Esc: Cancel"
en_dlg_title_search:  .asciz "Recursive Search"
en_srch_lbl_name:     .asciz "Name mask:"
en_srch_lbl_grep:     .asciz "Content (grep):"
en_srch_hint:         .asciz "Tab: Switch field • Enter: Search • Esc: Cancel"
en_fp_search_badge:   .asciz " [Search]"
en_msg_search_none:   .asciz "No matches found"
en_dlg_title_info:    .asciz "File Info"
en_dlg_title_chmod:   .asciz "Permissions (chmod/chown)"
en_dlg_title_symlink: .asciz "Create Symlink"
en_dlg_title_hardlink:.asciz "Create Hardlink"
en_prm_link_target:   .asciz "Link path:"
en_chm_hint:          .asciz "1-9/Space: Bit • u/g: uid:gid • Enter: Apply"
en_msg_trashed:       .asciz "Moved to Trash"
en_msg_undone:        .asciz "Undone"
en_msg_undo_empty:    .asciz "Nothing to undo"
en_dlg_title_palette: .asciz "Command Palette"
en_dlg_title_fuzzy:   .asciz "Fuzzy Finder"
en_fuz_hint:          .asciz "Type: Filter   Up/Dn: Select   Enter: Run/Jump   Esc: Close"
en_fp_du_badge:       .asciz "Disk Usage"
en_br_title:          .asciz "Bulk Rename"
en_br_tmpl:           .asciz "Template:"
en_br_find:           .asciz "Find:"
en_br_repl:           .asciz "Replace:"
en_ctx_view:          .asciz "View"
en_ctx_copy:          .asciz "Copy"
en_ctx_move:          .asciz "Move"
en_ctx_copy_path:     .asciz "Copy Path"
en_ctx_delete:        .asciz "Delete"
en_msg_copied_path:   .asciz "Copied path to clipboard"
en_diff_pfx:          .asciz "Diff: "
en_term_title:        .asciz "Terminal"
en_img_unsup:         .asciz "Image format not supported (Alt+V: Hex / Text)"
en_dlg_title_viewas:  .asciz "Open as"
en_vas_image:         .asciz "Image"
en_vas_hex:           .asciz "Hex"
en_vas_text:          .asciz "Text"
en_vas_hint:          .asciz "i: Image  h: Hex  t: Text  Esc: Cancel"
en_vw_img_hint:       .asciz "+/-:Zoom 1:100% 0:Fit b:Braille o:Open Arrows:Pan h:Hex t:Text Esc"
en_img_fit:           .asciz "Fit"


# Command Palette (Ctrl+K)
en_pal_theme:    .asciz "Theme: Cycle Color Theme (Alt+T)"
en_pal_lang:     .asciz "Language: Next Interface Language (Alt+L)"
en_pal_preview:  .asciz "Preview: Toggle Pane (F9)"
en_pal_hidden:   .asciz "Dotfiles: Toggle Hidden (.)"
en_pal_help:     .asciz "Help: Show Keybindings (F1)"
en_pal_rename:   .asciz "Rename: Rename Entry (F2)"
en_pal_view:     .asciz "View: Open File Viewer (F3)"
en_pal_edit:     .asciz "Edit: Open in $EDITOR (F4)"
en_pal_copy:     .asciz "Copy: Copy to Opposite Pane (F5)"
en_pal_move:     .asciz "Move: Move to Opposite Pane (F6)"
en_pal_mkdir:    .asciz "Mkdir: Create Directory (F7)"
en_pal_delete:   .asciz "Delete: Permanent Delete (F8)"
en_pal_newfile:  .asciz "New File: Create Empty File (Ctrl+N)"
en_pal_filter:   .asciz "Filter: Live Filter (/)"
en_pal_goto:     .asciz "Go To: Jump to Path (Ctrl+G)"
en_pal_refresh:  .asciz "Refresh: Reload Panes (Ctrl+R)"
en_pal_usermenu: .asciz "User Menu: Custom Commands (Alt+U)"
en_pal_shell:    .asciz "Shell: Suspend to Shell (Ctrl+O)"
en_pal_quit:     .asciz "Quit: Exit Duop (F10)"

# Command line (duop --version / --help)
en_cli_version:
    .ascii "Duop v"
    DUOP_VER_ASCII
    .ascii " (x86-64 Linux ELF64, zero-libc)\n"
    .ascii "Copyright (C) 2026 Kostiantyn Nedbailo (Недбайло Костянтин Геннадійович) <ChessPlayerSedoy@gmail.com>\n"
    .ascii "License: MIT License (Open Source — run with --license for full text)\n"
    .byte 0
en_cli_usage:
    .ascii "Duop v"
    DUOP_VER_ASCII
    .ascii " — Modern Dual-Pane Terminal File Manager (Pure x86-64 Assembly)\n"
    .ascii "Usage: duop [--lang ID] [--choosedir=FILE] [-V|--version] [-L|--license] [LEFT_DIR [RIGHT_DIR]]\n"
    .ascii "\n"
    .ascii "Features:\n"
    .ascii "  • 24-bit TrueColor UI with 3 switchable themes (Alt+T)\n"
    .ascii "  • Interface languages: built-in English + lang/*.lang packs (Alt+L, --lang ID, LANG=xx_*)\n"
    .ascii "  • Full SGR 1006 Mouse support (click, double-click, right-click, wheel, drag, hover)\n"
    .ascii "  • Dual-pane navigation & Live Preview mode (F9 / Ctrl+P)\n"
    .ascii "  • Built-in Text & Hex Viewer (F3), Copy/Move/Rename/Mkdir/Delete, Live Filter (/)\n"
    .ascii "\n"
    .ascii "Author: Kostiantyn Nedbailo (Недбайло Костянтин Геннадійович) <ChessPlayerSedoy@gmail.com>\n"
    .ascii "License: MIT License (Open Source)\n"
    .ascii "Press F1 inside Duop for full keyboard and mouse shortcuts.\n"
    .byte 0
en_help_tab_overview: .asciz " Overview "
en_help_tab_instr:    .asciz " Instruction "
en_hi_1:  .asciz "◆ FUNCTION KEYS (F1-F10)"
en_hi_2:  .asciz "  F1  Help / Instruction    F2  Rename          F3  View (Img/Hex/Txt)"
en_hi_3:  .asciz "  F4  Edit ($EDITOR)        F5  Copy            F6  Move"
en_hi_4:  .asciz "  F7  Mkdir                 F8 / Del  Delete    F9  Toggle Preview"
en_hi_5:  .asciz "  F10 Quit Duop"
en_hi_6:  .asciz "◆ NAVIGATION & PANES"
en_hi_7:  .asciz "  Up/Down, k/j, PgUp/PgDn   Move cursor / scroll page"
en_hi_8:  .asciz "  Home/g, End/G             Jump to first / last entry"
en_hi_9:  .asciz "  Enter                     Open dir or archive (.tar, .tar.gz, .zip)"
en_hi_10: .asciz "  Backspace / ~             Parent directory (..) / Home ($HOME)"
en_hi_11: .asciz "  Tab / Shift+Tab           Switch active pane (when Preview is off)"
en_hi_12: .asciz "  Alt+Left / Alt+Right      Directory history: back / forward"
en_hi_13: .asciz "  Esc                       Cancel loading or clear live filter"
en_hi_14: .asciz "◆ SELECTION, SEARCH & FILTER"
en_hi_15: .asciz "  Space / Insert            Toggle mark on current file"
en_hi_16: .asciz "  * / Ctrl+A                Toggle mark on all files"
en_hi_17: .asciz "  /                         Live filename filter"
en_hi_18: .asciz "  . / Ctrl+H                Show / hide dotfiles"
en_hi_19: .asciz "  z / Ctrl+F                Frecency jump / Search files & grep"
en_hi_20: .asciz "  Ctrl+K / Alt+P            Command Palette / Fuzzy Finder"
en_hi_21: .asciz "◆ TABS, UI & TOOLS"
en_hi_22: .asciz "  Ctrl+T / Ctrl+W           New tab / Close current tab"
en_hi_23: .asciz "  Ctrl+G / Ctrl+D           Go to path / Bookmarks"
en_hi_24: .asciz "  Ctrl+N / Ctrl+R / Ctrl+Z  New file / Refresh panes / Undo trash"
en_hi_25: .asciz "  Ctrl+O / Ctrl+` / Ctrl+P  External $SHELL / Embedded terminal / Preview"
en_hi_26: .asciz "  Ctrl+Q / Ctrl+C / q       Quit Duop"
en_hi_27: .asciz "  Alt+T / Alt+L / Alt+V     Cycle Theme / Language / Preview Mode"
en_hi_28: .asciz "  Alt+D / Alt+= / Alt+B     Disk Usage / Dir Diff / Bulk Rename"
en_hi_29: .asciz "  Alt+U / Alt+J             User Menu / Background Tasks"
en_hi_30: .asciz "  Alt+A / Alt+I             chmod / File Info"
en_hi_31: .asciz "  Alt+S / Alt+H             Create symlink / hardlink"
en_hi_32: .asciz "◆ VIEWER (F3) & HEX EDITOR"
en_hi_33: .asciz "  Up/Dn/PgUp/PgDn/Space     Scroll; Home/End: top/bottom; H: Text/Hex"
en_hi_34: .asciz "  Image: +/-, 1(100%), 0/f(Fit), Arrows(Pan), B(Braille), O(Open)"
en_hi_35: .asciz "  Hex Edit: E(Toggle edit), 0-9a-f(Nibbles), Ctrl+Z(Undo), Ctrl+S(Save)"
en_hi_36: .asciz "  Esc / q / F3 / F10        Close viewer (Tab/Arrows switch Help tabs)"

# Disk Usage progress window
en_du_scan_title:  .asciz "Scanning disk usage"
en_du_lbl_folders: .asciz "Folders"
en_du_lbl_scanned: .asciz "Scanned"
en_du_unit_dirs:   .asciz "dirs"
en_du_unit_files:  .asciz "files"
en_du_scan_hint:   .asciz "Esc: Cancel"
en_msg_du_canceled: .asciz "Disk usage scan canceled"
en_pkg_title:          .asciz " Package Manager (Alt+K) "
en_pkg_hint:           .asciz " Enter:Run/Install  i:Install  d:Remove  v:Verify  u:Upgrade  Tab:Switch "
en_pkg_empty:          .asciz " No packages installed "
en_pkg_install_prompt: .asciz " Package path or URL (.tar.gz/.zip): "
en_pkg_installed:      .asciz " Package installed "
en_pkg_removed:        .asciz " Package removed "
en_pkg_verified:       .asciz " Package verified (SHA-256 OK) "
en_pkg_err:            .asciz " Package operation failed "
en_pkg_tab_inst:       .asciz " Installed "
en_pkg_tab_avail:      .asciz " Available "
en_arc_truncated:      .asciz "Archive is damaged or incomplete: showing what could be read"
en_arc_readonly:       .asciz "Archive is read-only: copy, move and delete are not available inside it"
en_arc_member_unsupported: .asciz "This archive member is encrypted or uses an unsupported compression method"
en_arc_member_bad:     .asciz "Cannot read this archive member: its data is damaged or fails the checksum"
en_app_title_unpack: .asciz "Unpack"
en_app_prm_unpack: .asciz "Unpack destination path:"
en_task_title_unpack: .asciz "Unpacking"
en_task_title_arctest: .asciz "Testing archive"
en_app_ok_unpack: .asciz "Unpacked successfully"
en_app_ok_arctest: .asciz "Archive is OK: every member decoded and verified"
en_arc_issues_pfx: .asciz "Finished, but "
en_arc_issues_sfx: .asciz " member(s) could not be used (unsafe, encrypted, unsupported or damaged)"
en_arc_bomb: .asciz "Refused: the archive would expand more than 1000 times (decompression bomb?)"
en_arc_not_archive: .asciz "Not a readable archive"
en_arc_too_big: .asciz "Archive is too large to browse here (a .tar.gz / .tar.zst may unpack to 512 MiB at most): unpack it with Alt+F9"
en_arc_format_unsupported: .asciz "This archive format cannot be browsed yet (supported: .tar, .gz, .zst, .tar.gz, .tar.zst, .zip and zip-based files); F3 shows the raw file"
en_err_cmd_long:       .asciz "Command too long: not executed"
en_cfg_truncated:      .asciz "Config file too large: the rest was ignored"
en_err_special:        .asciz "Special file (pipe/socket/device) cannot be copied or moved this way"

.align 8
.global lang_en_table
lang_en_table:
    .quad en_code, en_name
    .quad en_hdr_brand, en_hdr_mode_dual, en_hdr_mode_prev, en_hdr_btn_quit, en_hdr_btn_help, en_hdr_btn_lang
    .quad en_hdr_btn_hid_on, en_hdr_btn_hid_off, en_hdr_btn_prev, en_hdr_theme_pfx
    .quad en_flbl_1, en_flbl_2, en_flbl_3, en_flbl_4, en_flbl_5
    .quad en_flbl_6, en_flbl_7, en_flbl_8, en_flbl_9, en_flbl_0
    .quad en_sb_dir_tag, en_sb_filter_pfx, en_sb_filter_sfx, en_sb_hint_right, en_msg_lang_sw
    .quad en_fp_col_name, en_fp_col_size, en_fp_col_mod, en_fp_col_perm, en_fp_dir_badge
    .quad en_fp_sel_pfx, en_fp_sel_mid, en_fp_sel_end, en_fp_sum_dirs, en_fp_sum_files, en_fp_flt_tag
    .quad en_pv_title, en_pv_dir_msg, en_pv_bin_hdr, en_pv_empty, en_pv_sz_lbl, en_pv_perm_lbl
    .quad en_err_perm, en_err_exists, en_err_noent, en_err_notempty, en_err_isdir, en_err_notdir, en_err_same, en_err_generic
    .quad en_dlg_btn_ok, en_dlg_btn_cancel, en_dlg_btn_yes, en_dlg_btn_no, en_dlg_btn_close, en_dlg_btn_hex
    .quad en_dlg_title_help, en_dlg_title_view, en_dlg_vw_hint, en_dlg_hlp_hint
    .quad en_hl_0, en_hl_1, en_hl_2, en_hl_3, en_hl_4, en_hl_5, en_hl_6, en_hl_7, en_hl_8
    .quad en_hl_9, en_hl_10, en_hl_11, en_hl_12, en_hl_13, en_hl_14, en_hl_15, en_hl_16, en_hl_17
    .quad en_hl_18, en_hl_19, en_hl_20, en_hl_21
    .quad en_app_title_mkdir, en_app_prm_mkdir, en_app_title_newfile, en_app_prm_newfile
    .quad en_app_title_rename, en_app_prm_rename, en_app_title_copy, en_app_prm_copy
    .quad en_app_title_move, en_app_prm_move, en_app_title_filter, en_app_prm_filter
    .quad en_app_title_goto, en_app_prm_goto, en_app_title_delete, en_app_msg_del_one, en_app_msg_del_many
    .quad en_app_sel_items_sfx, en_app_ok_mkdir, en_app_ok_newfile, en_app_ok_rename
    .quad en_app_ok_copy, en_app_ok_move, en_app_ok_delete, en_app_ok_refresh, en_app_err_parent, en_app_err_isdir
    .quad en_fp_loading_pfx, en_fp_huge_badge, en_fp_abort_badge, en_app_load_aborted
    .quad en_task_title_copy, en_task_title_move, en_task_title_delete
    .quad en_task_btn_pause, en_task_btn_resume, en_task_btn_bg
    .quad en_task_lbl_files, en_task_lbl_eta, en_task_lbl_scan, en_task_lbl_paused, en_task_lbl_cancel
    .quad en_task_sb_hint, en_err_canceled, en_err_busy, en_app_title_quit_tk, en_app_msg_quit_tk
    .quad en_dlg_title_usermenu, en_um_hint
    .quad en_dlg_title_bm, en_bm_hint, en_dlg_title_fre, en_fre_hint
    .quad en_msg_bm_added, en_msg_hist_empty, en_msg_tab_last
    .quad en_dlg_title_conflict, en_cfl_btn_overwrite, en_cfl_btn_skip, en_cfl_btn_rename, en_cfl_lbl_forall, en_cfl_hint
    .quad en_dlg_title_search, en_srch_lbl_name, en_srch_lbl_grep, en_srch_hint, en_fp_search_badge, en_msg_search_none
    .quad en_dlg_title_info, en_dlg_title_chmod, en_dlg_title_symlink, en_dlg_title_hardlink, en_prm_link_target, en_chm_hint, en_msg_trashed, en_msg_undone, en_msg_undo_empty, en_dlg_title_palette, en_dlg_title_fuzzy, en_fuz_hint, en_fp_du_badge, en_br_title, en_br_tmpl, en_br_find, en_br_repl
    .quad en_ctx_view, en_ctx_copy, en_ctx_move, en_ctx_copy_path, en_ctx_delete, en_msg_copied_path, en_diff_pfx, en_term_title
    .quad en_img_unsup, en_dlg_title_viewas, en_vas_image, en_vas_hex, en_vas_text, en_vas_hint, en_vw_img_hint, en_img_fit
    .quad en_pal_theme, en_pal_lang, en_pal_preview, en_pal_hidden, en_pal_help, en_pal_rename, en_pal_view, en_pal_edit, en_pal_copy, en_pal_move
    .quad en_pal_mkdir, en_pal_delete, en_pal_newfile, en_pal_filter, en_pal_goto, en_pal_refresh, en_pal_usermenu, en_pal_shell, en_pal_quit
    .quad en_cli_version, en_cli_usage
    .quad en_help_tab_overview, en_help_tab_instr
    .quad en_hi_1, en_hi_2, en_hi_3, en_hi_4, en_hi_5, en_hi_6, en_hi_7, en_hi_8, en_hi_9
    .quad en_hi_10, en_hi_11, en_hi_12, en_hi_13, en_hi_14, en_hi_15, en_hi_16, en_hi_17, en_hi_18
    .quad en_hi_19, en_hi_20, en_hi_21, en_hi_22, en_hi_23, en_hi_24, en_hi_25, en_hi_26, en_hi_27
    .quad en_hi_28, en_hi_29, en_hi_30, en_hi_31, en_hi_32, en_hi_33, en_hi_34, en_hi_35, en_hi_36
    .quad en_du_scan_title, en_du_lbl_folders, en_du_lbl_scanned, en_du_unit_dirs, en_du_unit_files, en_du_scan_hint, en_msg_du_canceled
    .quad en_pkg_title, en_pkg_hint, en_pkg_empty, en_pkg_install_prompt
    .quad en_pkg_installed, en_pkg_removed, en_pkg_verified, en_pkg_err
    .quad en_pkg_tab_inst, en_pkg_tab_avail, en_arc_truncated, en_arc_readonly, en_arc_member_unsupported, en_arc_member_bad
    .quad en_app_title_unpack, en_app_prm_unpack, en_task_title_unpack, en_task_title_arctest, en_app_ok_unpack, en_app_ok_arctest, en_arc_issues_pfx, en_arc_issues_sfx, en_arc_bomb, en_arc_not_archive
    .quad en_arc_too_big, en_arc_format_unsupported
    .quad en_err_cmd_long, en_cfg_truncated, en_err_special
.if (. - lang_en_table) != LangStrings._size
    .error "lang_en_table size does not match LangStrings._size"
.endif
