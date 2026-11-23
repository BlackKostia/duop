# Frozen reference oracle: the hand-written asm that src/tui/i18n_keys.c (key table generated into build/i18n_en.c) replaced (tests/test_c_i18n_table.py diffs against it)
# Interface language pack keys: the name of every LangStrings slot as written in *.lang files
# ("key = value"); array fields are numbered from 1 (f_labels.1 = F1 ... help_lines.22).
# Generated from include/types.inc; the guard below fails the build when a slot has no key.

.intel_syntax noprefix
.include "types.inc"

.set lk_slots, 0
.macro LK name, off
    .short \off
    .asciz "\name"
    .set lk_slots, lk_slots + 1
.endm

.section .rodata
i18n_keys:
    LK code, LangStrings.code
    LK name, LangStrings.name
    LK hdr_brand, LangStrings.hdr_brand
    LK hdr_mode_dual, LangStrings.hdr_mode_dual
    LK hdr_mode_prev, LangStrings.hdr_mode_prev
    LK hdr_btn_quit, LangStrings.hdr_btn_quit
    LK hdr_btn_help, LangStrings.hdr_btn_help
    LK hdr_btn_lang, LangStrings.hdr_btn_lang
    LK hdr_btn_hid_on, LangStrings.hdr_btn_hid_on
    LK hdr_btn_hid_off, LangStrings.hdr_btn_hid_off
    LK hdr_btn_prev, LangStrings.hdr_btn_prev
    LK hdr_theme_pfx, LangStrings.hdr_theme_pfx
    LK f_labels.1, LangStrings.f_labels+0
    LK f_labels.2, LangStrings.f_labels+8
    LK f_labels.3, LangStrings.f_labels+16
    LK f_labels.4, LangStrings.f_labels+24
    LK f_labels.5, LangStrings.f_labels+32
    LK f_labels.6, LangStrings.f_labels+40
    LK f_labels.7, LangStrings.f_labels+48
    LK f_labels.8, LangStrings.f_labels+56
    LK f_labels.9, LangStrings.f_labels+64
    LK f_labels.10, LangStrings.f_labels+72
    LK sb_dir_tag, LangStrings.sb_dir_tag
    LK sb_filter_pfx, LangStrings.sb_filter_pfx
    LK sb_filter_sfx, LangStrings.sb_filter_sfx
    LK sb_hint_right, LangStrings.sb_hint_right
    LK msg_lang_switched, LangStrings.msg_lang_switched
    LK fp_col_name, LangStrings.fp_col_name
    LK fp_col_size, LangStrings.fp_col_size
    LK fp_col_mod, LangStrings.fp_col_mod
    LK fp_col_perm, LangStrings.fp_col_perm
    LK fp_dir_badge, LangStrings.fp_dir_badge
    LK fp_sel_pfx, LangStrings.fp_sel_pfx
    LK fp_sel_mid, LangStrings.fp_sel_mid
    LK fp_sel_end, LangStrings.fp_sel_end
    LK fp_sum_dirs, LangStrings.fp_sum_dirs
    LK fp_sum_files, LangStrings.fp_sum_files
    LK fp_flt_tag, LangStrings.fp_flt_tag
    LK pv_title, LangStrings.pv_title
    LK pv_dir_msg, LangStrings.pv_dir_msg
    LK pv_bin_hdr, LangStrings.pv_bin_hdr
    LK pv_empty, LangStrings.pv_empty
    LK pv_sz_lbl, LangStrings.pv_sz_lbl
    LK pv_perm_lbl, LangStrings.pv_perm_lbl
    LK err_perm, LangStrings.err_perm
    LK err_exists, LangStrings.err_exists
    LK err_noent, LangStrings.err_noent
    LK err_notempty, LangStrings.err_notempty
    LK err_isdir, LangStrings.err_isdir
    LK err_notdir, LangStrings.err_notdir
    LK err_same, LangStrings.err_same
    LK err_generic, LangStrings.err_generic
    LK dlg_btn_ok, LangStrings.dlg_btn_ok
    LK dlg_btn_cancel, LangStrings.dlg_btn_cancel
    LK dlg_btn_yes, LangStrings.dlg_btn_yes
    LK dlg_btn_no, LangStrings.dlg_btn_no
    LK dlg_btn_close, LangStrings.dlg_btn_close
    LK dlg_btn_hex, LangStrings.dlg_btn_hex
    LK dlg_title_help, LangStrings.dlg_title_help
    LK dlg_title_viewer, LangStrings.dlg_title_viewer
    LK dlg_vw_hint, LangStrings.dlg_vw_hint
    LK dlg_hlp_hint, LangStrings.dlg_hlp_hint
    LK help_lines.1, LangStrings.help_lines+0
    LK help_lines.2, LangStrings.help_lines+8
    LK help_lines.3, LangStrings.help_lines+16
    LK help_lines.4, LangStrings.help_lines+24
    LK help_lines.5, LangStrings.help_lines+32
    LK help_lines.6, LangStrings.help_lines+40
    LK help_lines.7, LangStrings.help_lines+48
    LK help_lines.8, LangStrings.help_lines+56
    LK help_lines.9, LangStrings.help_lines+64
    LK help_lines.10, LangStrings.help_lines+72
    LK help_lines.11, LangStrings.help_lines+80
    LK help_lines.12, LangStrings.help_lines+88
    LK help_lines.13, LangStrings.help_lines+96
    LK help_lines.14, LangStrings.help_lines+104
    LK help_lines.15, LangStrings.help_lines+112
    LK help_lines.16, LangStrings.help_lines+120
    LK help_lines.17, LangStrings.help_lines+128
    LK help_lines.18, LangStrings.help_lines+136
    LK help_lines.19, LangStrings.help_lines+144
    LK help_lines.20, LangStrings.help_lines+152
    LK help_lines.21, LangStrings.help_lines+160
    LK help_lines.22, LangStrings.help_lines+168
    LK app_title_mkdir, LangStrings.app_title_mkdir
    LK app_prm_mkdir, LangStrings.app_prm_mkdir
    LK app_title_newfile, LangStrings.app_title_newfile
    LK app_prm_newfile, LangStrings.app_prm_newfile
    LK app_title_rename, LangStrings.app_title_rename
    LK app_prm_rename, LangStrings.app_prm_rename
    LK app_title_copy, LangStrings.app_title_copy
    LK app_prm_copy, LangStrings.app_prm_copy
    LK app_title_move, LangStrings.app_title_move
    LK app_prm_move, LangStrings.app_prm_move
    LK app_title_filter, LangStrings.app_title_filter
    LK app_prm_filter, LangStrings.app_prm_filter
    LK app_title_goto, LangStrings.app_title_goto
    LK app_prm_goto, LangStrings.app_prm_goto
    LK app_title_delete, LangStrings.app_title_delete
    LK app_msg_del_one, LangStrings.app_msg_del_one
    LK app_msg_del_many, LangStrings.app_msg_del_many
    LK app_sel_items_sfx, LangStrings.app_sel_items_sfx
    LK app_ok_mkdir, LangStrings.app_ok_mkdir
    LK app_ok_newfile, LangStrings.app_ok_newfile
    LK app_ok_rename, LangStrings.app_ok_rename
    LK app_ok_copy, LangStrings.app_ok_copy
    LK app_ok_move, LangStrings.app_ok_move
    LK app_ok_delete, LangStrings.app_ok_delete
    LK app_ok_refresh, LangStrings.app_ok_refresh
    LK app_err_parent, LangStrings.app_err_parent
    LK app_err_isdir, LangStrings.app_err_isdir
    LK fp_loading_pfx, LangStrings.fp_loading_pfx
    LK fp_huge_badge, LangStrings.fp_huge_badge
    LK fp_abort_badge, LangStrings.fp_abort_badge
    LK app_load_aborted, LangStrings.app_load_aborted
    LK task_title_copy, LangStrings.task_title_copy
    LK task_title_move, LangStrings.task_title_move
    LK task_title_delete, LangStrings.task_title_delete
    LK task_btn_pause, LangStrings.task_btn_pause
    LK task_btn_resume, LangStrings.task_btn_resume
    LK task_btn_bg, LangStrings.task_btn_bg
    LK task_lbl_files, LangStrings.task_lbl_files
    LK task_lbl_eta, LangStrings.task_lbl_eta
    LK task_lbl_scan, LangStrings.task_lbl_scan
    LK task_lbl_paused, LangStrings.task_lbl_paused
    LK task_lbl_canceling, LangStrings.task_lbl_canceling
    LK task_sb_hint, LangStrings.task_sb_hint
    LK err_canceled, LangStrings.err_canceled
    LK err_busy, LangStrings.err_busy
    LK app_title_quit_task, LangStrings.app_title_quit_task
    LK app_msg_quit_task, LangStrings.app_msg_quit_task
    LK dlg_title_usermenu, LangStrings.dlg_title_usermenu
    LK um_hint, LangStrings.um_hint
    LK dlg_title_bookmarks, LangStrings.dlg_title_bookmarks
    LK bm_hint, LangStrings.bm_hint
    LK dlg_title_frecency, LangStrings.dlg_title_frecency
    LK fre_hint, LangStrings.fre_hint
    LK msg_bm_added, LangStrings.msg_bm_added
    LK msg_hist_empty, LangStrings.msg_hist_empty
    LK msg_tab_last, LangStrings.msg_tab_last
    LK dlg_title_conflict, LangStrings.dlg_title_conflict
    LK cfl_btn_overwrite, LangStrings.cfl_btn_overwrite
    LK cfl_btn_skip, LangStrings.cfl_btn_skip
    LK cfl_btn_rename, LangStrings.cfl_btn_rename
    LK cfl_lbl_forall, LangStrings.cfl_lbl_forall
    LK cfl_hint, LangStrings.cfl_hint
    LK dlg_title_search, LangStrings.dlg_title_search
    LK srch_lbl_name, LangStrings.srch_lbl_name
    LK srch_lbl_grep, LangStrings.srch_lbl_grep
    LK srch_hint, LangStrings.srch_hint
    LK fp_search_badge, LangStrings.fp_search_badge
    LK msg_search_none, LangStrings.msg_search_none
    LK dlg_title_info, LangStrings.dlg_title_info
    LK dlg_title_chmod, LangStrings.dlg_title_chmod
    LK dlg_title_symlink, LangStrings.dlg_title_symlink
    LK dlg_title_hardlink, LangStrings.dlg_title_hardlink
    LK prm_link_target, LangStrings.prm_link_target
    LK chm_hint, LangStrings.chm_hint
    LK msg_trashed, LangStrings.msg_trashed
    LK msg_undone, LangStrings.msg_undone
    LK msg_undo_empty, LangStrings.msg_undo_empty
    LK dlg_title_palette, LangStrings.dlg_title_palette
    LK dlg_title_fuzzy, LangStrings.dlg_title_fuzzy
    LK fuz_hint, LangStrings.fuz_hint
    LK fp_du_badge, LangStrings.fp_du_badge
    LK br_title, LangStrings.br_title
    LK br_tmpl, LangStrings.br_tmpl
    LK br_find, LangStrings.br_find
    LK br_repl, LangStrings.br_repl
    LK ctx_view, LangStrings.ctx_view
    LK ctx_copy, LangStrings.ctx_copy
    LK ctx_move, LangStrings.ctx_move
    LK ctx_copy_path, LangStrings.ctx_copy_path
    LK ctx_delete, LangStrings.ctx_delete
    LK msg_copied_path, LangStrings.msg_copied_path
    LK diff_pfx, LangStrings.diff_pfx
    LK term_title, LangStrings.term_title
    LK img_unsup, LangStrings.img_unsup
    LK dlg_title_viewas, LangStrings.dlg_title_viewas
    LK vas_image, LangStrings.vas_image
    LK vas_hex, LangStrings.vas_hex
    LK vas_text, LangStrings.vas_text
    LK vas_hint, LangStrings.vas_hint
    LK vw_img_hint, LangStrings.vw_img_hint
    LK img_fit, LangStrings.img_fit
    LK pal_theme, LangStrings.pal_theme
    LK pal_lang, LangStrings.pal_lang
    LK pal_preview, LangStrings.pal_preview
    LK pal_hidden, LangStrings.pal_hidden
    LK pal_help, LangStrings.pal_help
    LK pal_rename, LangStrings.pal_rename
    LK pal_view, LangStrings.pal_view
    LK pal_edit, LangStrings.pal_edit
    LK pal_copy, LangStrings.pal_copy
    LK pal_move, LangStrings.pal_move
    LK pal_mkdir, LangStrings.pal_mkdir
    LK pal_delete, LangStrings.pal_delete
    LK pal_newfile, LangStrings.pal_newfile
    LK pal_filter, LangStrings.pal_filter
    LK pal_goto, LangStrings.pal_goto
    LK pal_refresh, LangStrings.pal_refresh
    LK pal_usermenu, LangStrings.pal_usermenu
    LK pal_shell, LangStrings.pal_shell
    LK pal_quit, LangStrings.pal_quit
    LK cli_version, LangStrings.cli_version
    LK cli_usage, LangStrings.cli_usage
    LK help_tab_overview, LangStrings.help_tab_overview
    LK help_tab_instr, LangStrings.help_tab_instr
    LK help_instr_lines.1,  LangStrings.help_instr_lines+0
    LK help_instr_lines.2,  LangStrings.help_instr_lines+8
    LK help_instr_lines.3,  LangStrings.help_instr_lines+16
    LK help_instr_lines.4,  LangStrings.help_instr_lines+24
    LK help_instr_lines.5,  LangStrings.help_instr_lines+32
    LK help_instr_lines.6,  LangStrings.help_instr_lines+40
    LK help_instr_lines.7,  LangStrings.help_instr_lines+48
    LK help_instr_lines.8,  LangStrings.help_instr_lines+56
    LK help_instr_lines.9,  LangStrings.help_instr_lines+64
    LK help_instr_lines.10, LangStrings.help_instr_lines+72
    LK help_instr_lines.11, LangStrings.help_instr_lines+80
    LK help_instr_lines.12, LangStrings.help_instr_lines+88
    LK help_instr_lines.13, LangStrings.help_instr_lines+96
    LK help_instr_lines.14, LangStrings.help_instr_lines+104
    LK help_instr_lines.15, LangStrings.help_instr_lines+112
    LK help_instr_lines.16, LangStrings.help_instr_lines+120
    LK help_instr_lines.17, LangStrings.help_instr_lines+128
    LK help_instr_lines.18, LangStrings.help_instr_lines+136
    LK help_instr_lines.19, LangStrings.help_instr_lines+144
    LK help_instr_lines.20, LangStrings.help_instr_lines+152
    LK help_instr_lines.21, LangStrings.help_instr_lines+160
    LK help_instr_lines.22, LangStrings.help_instr_lines+168
    LK help_instr_lines.23, LangStrings.help_instr_lines+176
    LK help_instr_lines.24, LangStrings.help_instr_lines+184
    LK help_instr_lines.25, LangStrings.help_instr_lines+192
    LK help_instr_lines.26, LangStrings.help_instr_lines+200
    LK help_instr_lines.27, LangStrings.help_instr_lines+208
    LK help_instr_lines.28, LangStrings.help_instr_lines+216
    LK help_instr_lines.29, LangStrings.help_instr_lines+224
    LK help_instr_lines.30, LangStrings.help_instr_lines+232
    LK help_instr_lines.31, LangStrings.help_instr_lines+240
    LK help_instr_lines.32, LangStrings.help_instr_lines+248
    LK help_instr_lines.33, LangStrings.help_instr_lines+256
    LK help_instr_lines.34, LangStrings.help_instr_lines+264
    LK help_instr_lines.35, LangStrings.help_instr_lines+272
    LK help_instr_lines.36, LangStrings.help_instr_lines+280
    LK du_scan_title, LangStrings.du_scan_title
    LK du_lbl_folders, LangStrings.du_lbl_folders
    LK du_lbl_scanned, LangStrings.du_lbl_scanned
    LK du_unit_dirs, LangStrings.du_unit_dirs
    LK du_unit_files, LangStrings.du_unit_files
    LK du_scan_hint, LangStrings.du_scan_hint
    LK msg_du_canceled, LangStrings.msg_du_canceled
    LK pkg_title, LangStrings.pkg_title
    LK pkg_hint, LangStrings.pkg_hint
    LK pkg_empty, LangStrings.pkg_empty
    LK pkg_install_prompt, LangStrings.pkg_install_prompt
    LK pkg_installed, LangStrings.pkg_installed
    LK pkg_removed, LangStrings.pkg_removed
    LK pkg_verified, LangStrings.pkg_verified
    LK pkg_err, LangStrings.pkg_err
    LK pkg_tab_inst, LangStrings.pkg_tab_inst
    LK pkg_tab_avail, LangStrings.pkg_tab_avail
    LK arc_truncated, LangStrings.arc_truncated
    LK arc_readonly, LangStrings.arc_readonly
    LK arc_member_unsupported, LangStrings.arc_member_unsupported
    LK arc_member_bad, LangStrings.arc_member_bad
    LK err_cmd_long, LangStrings.err_cmd_long
    LK cfg_truncated, LangStrings.cfg_truncated
    LK err_special, LangStrings.err_special
    .short 0xFFFF
.if lk_slots * 8 != LangStrings._size
    .error "i18n_keys: every LangStrings slot needs exactly one key"
.endif

.section .text
.global i18n_key_offset

# int i18n_key_offset(const char *key) -> byte offset of the LangStrings slot, or -1
i18n_key_offset:
    push rbx
    push r12
    sub rsp, 8
    mov r12, rdi
    lea rbx, [rip + i18n_keys]
.Lko_next:
    movzx eax, word ptr [rbx]
    cmp eax, 0xFFFF
    je .Lko_none
    lea rdi, [rbx + 2]
    mov rsi, r12
    call str_cmp
    test eax, eax
    jz .Lko_hit
    lea rdi, [rbx + 2]
    call str_len
    lea rbx, [rbx + rax + 3]
    jmp .Lko_next
.Lko_hit:
    movzx eax, word ptr [rbx]
    jmp .Lko_ret
.Lko_none:
    mov eax, -1
.Lko_ret:
    add rsp, 8
    pop r12
    pop rbx
    ret
