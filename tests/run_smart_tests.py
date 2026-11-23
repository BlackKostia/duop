#!/usr/bin/env python3
"""Smart selective test runner for Duop.

Selects and runs only the test suites affected by modified/staged files,
falling back to the full Makefile `test` suite when core headers, runtime,
or build files change (or when --all is passed).
"""

from __future__ import annotations

import argparse
import fnmatch
import re
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# Patterns that impact the entire binary and require running the full suite.
FULL_SUITE_PATTERNS = (
    "include/*",
    "Makefile",
    "src/core/*",
    "src/os/*",
    "src/app/main.c",
    "src/app/app.c",
    "src/app/app_loop.c",
    "src/app/app_events.c",
)

# A module migration only *registers* itself in the Makefile (a source in SRCS / C_SRCS, a test in `test:`) and appends
# declarations to include/duop_*.h. Such hunks cannot change how the rest of the tree builds, so they do not need the
# full suite (tiered pre-commit hook: the full `make test` runs in pre-push, before tags and in CI).
REGISTRATION_LINE = re.compile(r"^\s*(?:src/\S+\.(?:s|c)(?:\s*\\)?|python3 tests/test_\w+\.py)\s*$")
LOCAL_HEADER_GLOB = "include/duop_*.h"
ABI_TESTS = ("tests/test_abi_layouts.py", "tests/test_abi_cross.py")


# Subsystem path globs -> affected test scripts (relative to ROOT).
PATH_TEST_RULES: tuple[tuple[str, tuple[str, ...]], ...] = (
    ("src/fs/file_ops_overwrite*", ("tests/test_c_fops_overwrite.py", "tests/test_copy_overwrite_atomic.py", "tests/test_fs_reliability.py")),
    ("src/fs/file_ops_read*", ("tests/test_c_fops_small.py", "tests/test_viewer_pro.py", "tests/test_fs_reliability.py")),
    ("src/fs/file_ops_copy*", ("tests/test_c_file_ops_copy.py", "tests/test_fs_reliability.py", "tests/test_bg_tasks.py", "tests/test_reliability_audit.py", "tests/test_copy_overwrite_atomic.py")),
    ("src/fs/file_ops_delete*", ("tests/test_c_file_ops_delete.py", "tests/test_fs_reliability.py", "tests/test_bg_tasks.py", "tests/test_c_trash.py")),
    ("src/fs/file_ops.c", ("tests/test_c_file_ops.py", "tests/test_fs_reliability.py", "tests/test_bg_tasks.py", "tests/test_copy_overwrite_atomic.py")),
    ("src/fs/file_ops*", ("tests/test_fs_reliability.py", "tests/test_bg_tasks.py", "tests/test_reliability_audit.py", "tests/test_copy_overwrite_atomic.py")),
    ("src/fs/task_conflict*", ("tests/test_c_task_conflict.py", "tests/test_fs_reliability.py", "tests/test_reliability_audit.py", "tests/test_security_fixes.py")),
    ("src/fs/task_control*", ("tests/test_c_task_control.py", "tests/test_bg_tasks.py", "tests/test_fs_reliability.py")),
    ("src/fs/task.c", ("tests/test_c_task.py", "tests/test_bg_tasks.py", "tests/test_fs_reliability.py")),
    ("src/fs/task_worker*", ("tests/test_c_task.py", "tests/test_bg_tasks.py", "tests/test_fs_reliability.py")),
    ("src/fs/task*", ("tests/test_fs_reliability.py", "tests/test_bg_tasks.py", "tests/test_reliability_audit.py", "tests/test_copy_overwrite_atomic.py")),
    ("src/app/app_task*", ("tests/test_c_app_task.py", "tests/test_fs_reliability.py", "tests/test_bg_tasks.py", "tests/test_reliability_audit.py")),
    ("src/ui/progress_dialog*", ("tests/test_c_progress_dialog.py", "tests/test_bg_tasks.py", "tests/test_pty.py")),
    ("src/ui/conflict_dialog*", ("tests/test_fs_reliability.py", "tests/test_reliability_audit.py")),
    ("src/fs/dir_model_load*", ("tests/test_c_dir_model_load.py", "tests/test_performance_dir.py", "tests/test_fastpath.py", "tests/test_event_loop.py")),
    ("src/fs/dir_model_stream*", ("tests/test_c_dir_model_load.py", "tests/test_performance_dir.py", "tests/test_fastpath.py", "tests/test_event_loop.py")),
    ("src/fs/dir_model_filter*", ("tests/test_c_dir_model_filter.py", "tests/test_nav_viewer.py", "tests/test_pty.py", "tests/test_fastpath.py")),
    ("src/fs/dir_model.c", ("tests/test_c_dir_model.py", "tests/test_performance_dir.py", "tests/test_pty.py")),
    ("src/fs/dir_model_s*", ("tests/test_c_dir_model_stat.py", "tests/test_performance_dir.py", "tests/test_fastpath.py")),
    ("src/fs/dir_model*", ("tests/test_performance_dir.py", "tests/test_fastpath.py", "tests/test_nav_viewer.py", "tests/test_event_loop.py")),
    ("src/fs/dir_sort*", ("tests/test_c_dir_sort.py", "tests/test_performance_dir.py", "tests/test_pty.py")),
    ("src/fs/dir_cache*", ("tests/test_c_dir_cache.py", "tests/test_nav_viewer.py", "tests/test_performance_dir.py")),
    ("src/fs/dir_watch*", ("tests/test_event_loop.py", "tests/test_c_dir_watch.py")),
    ("src/ui/file_pane_layout*", ("tests/test_c_file_pane_layout.py", "tests/test_pty.py", "tests/test_ui_visual.py")),
    ("src/ui/file_pane_tabs*", ("tests/test_c_file_pane_tabs.py", "tests/test_nav_tabs.py", "tests/test_pty.py")),
    ("src/ui/file_pane_keys*", ("tests/test_c_file_pane_keys.py", "tests/test_pty.py", "tests/test_pane_scrollbar.py")),
    ("src/ui/file_pane_mouse*", ("tests/test_c_file_pane_mouse.py", "tests/test_pty.py", "tests/test_pane_scrollbar.py", "tests/test_nav_tabs.py")),
    ("src/ui/file_pane_render*", ("tests/test_c_file_pane_render.py", "tests/test_ui_visual.py", "tests/test_pane_scrollbar.py", "tests/test_pty.py")),
    ("src/ui/file_pane_rows*", ("tests/test_c_file_pane_render.py", "tests/test_ui_visual.py", "tests/test_pane_scrollbar.py", "tests/test_pty.py")),
    ("src/ui/file_pane*", ("tests/test_c_file_pane.py", "tests/test_pty.py", "tests/test_ui_visual.py", "tests/test_nav_tabs.py", "tests/test_pane_scrollbar.py")),
    ("src/fs/pkg_registry*", ("tests/test_c_pkg_registry.py", "tests/test_pkg_manager.py", "tests/test_pkg_improvements.py")),
    ("src/fs/pkg_verify*", ("tests/test_c_pkg_verify.py", "tests/test_pkg_manager.py", "tests/test_security_reliability.py")),
    ("src/fs/pkg_popup*", ("tests/test_c_pkg_popup.py", "tests/test_pkg_extensions.py")),
    ("src/fs/pkg_preview*", ("tests/test_c_pkg_popup.py", "tests/test_c_preview.py", "tests/test_pkg_improvements.py")),
    ("src/fs/pkg_vfs_pane*", ("tests/test_c_pkg_vfs_pane.py", "tests/test_pkg_extensions.py", "tests/test_pkg_improvements.py")),
    ("src/fs/pkg_vfs.*", ("tests/test_c_pkg_vfs.py", "tests/test_pkg_extensions.py", "tests/test_audit_regressions_sig.py")),
    ("src/fs/pkg_actions*", ("tests/test_c_pkg_actions.py", "tests/test_pkg_extensions.py", "tests/test_pkg_improvements.py")),
    ("src/fs/pkg_cli*", ("tests/test_c_pkg_cli.py", "tests/test_pkg_manager.py", "tests/test_pkg_improvements.py")),
    ("src/fs/pkg_manager*", ("tests/test_c_pkg_manager.py", "tests/test_pkg_manager.py", "tests/test_pkg_extensions.py", "tests/test_pkg_improvements.py")),
    ("src/fs/pkg_archive*", ("tests/test_c_pkg_archive.py", "tests/test_pkg_manager.py", "tests/test_pkg_extensions.py")),
    ("src/fs/pkg_fetch*", ("tests/test_c_pkg_fetch.py", "tests/test_pkg_manager.py", "tests/test_pkg_improvements.py")),
    ("src/fs/pkg_*", ("tests/test_pkg_manager.py", "tests/test_pkg_extensions.py", "tests/test_pkg_improvements.py")),
    ("src/ui/pkg_dialog*", ("tests/test_c_pkg_dialog.py", "tests/test_pkg_manager.py", "tests/test_pkg_extensions.py", "tests/test_pkg_improvements.py")),
    ("src/app/app_menu*", ("tests/test_c_app_menu_open.py", "tests/test_pkg_extensions.py", "tests/test_pkg_improvements.py", "tests/test_cmd_expand_safety.py")),
    ("src/app/app_cmdline*", ("tests/test_c_app_du_cmd.py", "tests/test_cmd_expand_safety.py")),
    ("src/fs/img_core*", ("tests/test_c_img_core.py", "tests/test_c_preview.py", "tests/test_img_preview.py", "tests/test_image_formats.py")),
    ("src/fs/img_*", ("tests/test_img_preview.py", "tests/test_image_formats.py", "tests/test_image_quality.py", "tests/test_decoder_bounds.py")),
    ("src/fs/preview_*", ("tests/test_c_preview.py", "tests/test_img_preview.py", "tests/test_event_loop.py", "tests/test_medium_fixes.py")),
    ("src/ui/img_overlay*", ("tests/test_c_img_overlay.py", "tests/test_img_preview.py", "tests/test_image_quality.py")),
    ("src/ui/img_preview_api*", ("tests/test_c_img_preview_api.py", "tests/test_img_preview.py", "tests/test_image_quality.py")),
    ("src/ui/img_preview.c", ("tests/test_c_img_cells.py", "tests/test_img_preview.py", "tests/test_image_quality.py")),
    ("src/ui/img_quad*", ("tests/test_c_img_cells.py", "tests/test_img_preview.py", "tests/test_image_quality.py")),
    ("src/ui/img_braille*", ("tests/test_c_img_cells.py", "tests/test_img_preview.py", "tests/test_image_quality.py")),
    ("src/ui/img_*", ("tests/test_img_preview.py", "tests/test_image_quality.py")),
    ("src/ui/preview_mode*", ("tests/test_c_preview_mode.py", "tests/test_img_preview.py", "tests/test_ui_visual.py")),
    ("src/ui/preview_pane_mouse*", ("tests/test_c_ui_leaf1.py", "tests/test_ui_visual.py")),
    ("src/ui/preview_pane*", ("tests/test_c_preview_pane.py", "tests/test_img_preview.py", "tests/test_ui_visual.py", "tests/test_pty.py")),
    ("src/ui/preview_*", ("tests/test_img_preview.py", "tests/test_ui_visual.py")),
    ("src/fs/disk_usage*", ("tests/test_disk_usage.py", "tests/test_du_progress.py", "tests/test_du_sizes.py", "tests/test_c_du_populate.py")),
    ("src/fs/du_*", ("tests/test_disk_usage.py", "tests/test_du_progress.py", "tests/test_du_sizes.py", "tests/test_c_du_tables.py", "tests/test_c_du_walk.py")),
    ("src/ui/du_render*", ("tests/test_c_git_du_render.py", "tests/test_disk_usage.py", "tests/test_du_progress.py", "tests/test_du_sizes.py")),
    ("src/ui/du_progress_dlg*", ("tests/test_c_du_progress_dlg.py", "tests/test_disk_usage.py", "tests/test_du_progress.py", "tests/test_du_sizes.py")),
    ("src/ui/du_*", ("tests/test_disk_usage.py", "tests/test_du_progress.py", "tests/test_du_sizes.py")),
    ("src/app/app_du*", ("tests/test_c_app_du_cmd.py", "tests/test_disk_usage.py", "tests/test_du_progress.py", "tests/test_du_sizes.py")),
    ("src/fs/archive_*", ("tests/test_c_archive.py", "tests/test_c_arc_vfs.py", "tests/test_archive_vfs.py", "tests/test_archive_hardening.py", "tests/test_arc_fuzz.py", "tests/test_pkg_manager.py")),
    ("include/duop_arc.h", ("tests/test_c_archive.py", "tests/test_archive_vfs.py")),
    ("src/fs/inflate_stream*", ("tests/test_c_inflate_stream.py", "tests/test_c_gzip_dec.py")),
    ("src/fs/gzip_dec*", ("tests/test_c_gzip_dec.py",)),
    ("src/fs/arc_index*", ("tests/test_c_arc_index.py", "tests/test_c_arc_tar.py")),
    ("src/fs/arc_tar*", ("tests/test_c_arc_tar.py",)),
    ("src/fs/arc_zip*", ("tests/test_c_arc_zip.py", "tests/test_c_arc_vfs.py")),
    ("src/fs/arc_source*", ("tests/test_c_arc_vfs.py", "tests/test_archive_vfs.py")),
    ("src/fs/arc_extract*", ("tests/test_c_arc_vfs.py", "tests/test_archive_vfs.py", "tests/test_archive_hardening.py")),
    ("include/duop_arcparse.h", ("tests/test_c_arc_tar.py", "tests/test_c_arc_zip.py")),
    ("include/duop_arcidx.h", ("tests/test_c_arc_index.py",)),
    ("src/fs/inflate*", ("tests/test_c_inflate.py", "tests/test_archive_vfs.py", "tests/test_pkg_manager.py")),
    ("src/app/app_arc*", ("tests/test_c_app_leaf1.py", "tests/test_archive_vfs.py", "tests/test_arc_readonly.py")),
    ("src/fs/git_status*", ("tests/test_c_git_status.py", "tests/test_git_status.py", "tests/test_git_trust.py")),
("src/fs/git_*", ("tests/test_git_status.py", "tests/test_git_trust.py")),
    ("src/ui/git_render*", ("tests/test_c_git_du_render.py", "tests/test_git_status.py")),
    ("src/ui/header_bar_git*", ("tests/test_c_ui_leaf1.py", "tests/test_git_status.py")),
    ("src/fs/nav_state*", ("tests/test_nav_tabs.py", "tests/test_nav_viewer.py", "tests/test_choosedir.py", "tests/test_c_nav_state.py")),
    ("src/fs/nav_persist*", ("tests/test_nav_tabs.py", "tests/test_nav_viewer.py", "tests/test_choosedir.py", "tests/test_c_nav_state.py", "tests/test_medium_fixes.py")),
    ("src/app/app_choosedir*", ("tests/test_c_app_leaf1.py", "tests/test_choosedir.py")),
    ("src/fs/dir_search*", ("tests/test_search_virtual.py", "tests/test_crash_regressions.py")),
    ("src/fs/dir_fuzzy*", ("tests/test_fuzzy_palette.py", "tests/test_c_dir_fuzzy.py")),
    ("src/fs/fs_write*", ("tests/test_c_dir_fuzzy.py", "tests/test_audit_regressions_io.py")),
    ("src/ui/search_dialog*", ("tests/test_search_virtual.py",)),
    ("src/ui/fuzzy_dialog*", ("tests/test_fuzzy_palette.py",)),
    ("src/fs/dir_diff*", ("tests/test_diff_menu_clip.py",)),
    ("src/ui/popup_osc52*", ("tests/test_c_ui_leaf1.py", "tests/test_diff_menu_clip.py")),
    ("src/ui/popup_menu*", ("tests/test_c_popup_menu.py", "tests/test_diff_menu_clip.py", "tests/test_pkg_extensions.py")),
    ("src/fs/trash_*", ("tests/test_trash_undo_attr.py", "tests/test_trash_spec.py", "tests/test_c_trash.py")),
    ("src/app/app_attr*", ("tests/test_c_app_attr.py", "tests/test_trash_undo_attr.py")),
    ("src/ui/attr_dialogs*", ("tests/test_trash_undo_attr.py",)),
    ("src/ui/info_dialog*", ("tests/test_trash_undo_attr.py",)),
    ("src/ui/viewer_scrollbar*", ("tests/test_c_viewer_scrollbar.py", "tests/test_viewer_pro.py", "tests/test_nav_viewer.py")),
    ("src/ui/viewer_dialog*", ("tests/test_c_viewer_dialog.py", "tests/test_viewer_pro.py", "tests/test_nav_viewer.py", "tests/test_pty.py")),
    ("src/ui/viewer_image*", ("tests/test_c_viewer_image.py", "tests/test_viewer_pro.py", "tests/test_nav_viewer.py", "tests/test_img_preview.py")),
    ("src/ui/viewer_pixels*", ("tests/test_c_viewer_pixels.py", "tests/test_viewer_pro.py", "tests/test_nav_viewer.py", "tests/test_img_preview.py")),
    ("src/ui/viewer_zoom*", ("tests/test_c_viewer_zoom.py", "tests/test_viewer_pro.py", "tests/test_nav_viewer.py")),
    ("src/ui/viewer_*", ("tests/test_viewer_pro.py", "tests/test_nav_viewer.py")),
    ("src/ui/view_as_dialog*", ("tests/test_viewer_pro.py",)),
    ("src/ui/syntax*", ("tests/test_c_syntax.py", "tests/test_viewer_pro.py")),
    ("src/ui/hex_edit*", ("tests/test_c_hex_edit.py", "tests/test_viewer_pro.py")),
    ("src/ui/elf_inspect*", ("tests/test_c_elf_inspect.py", "tests/test_viewer_pro.py", "tests/test_audit_regressions_parsers.py")),
    ("src/app/app_view*", ("tests/test_c_app_leaf1.py", "tests/test_viewer_pro.py", "tests/test_nav_viewer.py")),
    ("src/ui/bulk_rename*", ("tests/test_c_bulk_rename.py", "tests/test_bulk_rename.py", "tests/test_medium_fixes.py", "tests/test_pty.py")),
    ("src/ui/term_keys*", ("tests/test_c_ui_leaf1.py", "tests/test_term_pane.py")),
    ("src/ui/term_pane_input*", ("tests/test_c_ui_leaf1.py", "tests/test_term_pane.py")),
    ("src/ui/term_pane*", ("tests/test_c_term_pane.py", "tests/test_term_pane.py", "tests/test_crash_regressions.py")),
    ("src/ui/term_*", ("tests/test_term_pane.py", "tests/test_crash_regressions.py")),
    ("src/ui/user_menu*", ("tests/test_config_usermenu.py",)),
    ("src/ui/bookmark*", ("tests/test_nav_tabs.py",)),
    ("src/tui/i18n*", ("tests/test_lang_packs.py", "tests/test_i18n_hook.py", "tests/test_c_i18n.py", "tests/test_c_i18n_table.py")),
    ("include/duop_app.h", ("tests/test_c_app_api.py",)),
    ("src/app/*", ("tests/test_c_app_api.py",)),
    ("src/core/memcheck*", ("tests/test_c_memcheck.py", "tests/test_memcheck.py")),
    ("tests/harness/memcheck_*", ("tests/test_c_memcheck.py",)),
    ("tests/c_harness.py", ("tests/test_c_harness_utils.py", "tests/test_c_i18n_table.py", "tests/test_c_syscall.py", "tests/test_c_hash.py")),
    ("tests/run_tests.py", ("tests/test_run_tests.py",)),
    ("tests/run_smart_tests.py", ("tests/test_smart_runner.py",)),
    ("tests/harness/script_cli.h", ("tests/test_c_harness_utils.py",)),
    ("tests/harness/ui_event.h", ("tests/test_c_ui_leaf1.py",)),
    ("tests/harness/ui_app_stub.h", ("tests/test_c_ui_leaf1.py",)),
    ("src/app/cli_args*", ("tests/test_headless.py",)),
    ("VERSION", ("tests/test_headless.py",)),
    ("src/app/license*", ("tests/test_headless.py",)),
    ("src/app/main_headless*", ("tests/test_headless.py",)),
    ("include/duop_cli.h", ("tests/test_headless.py",)),
    ("tests/harness/cli_parse_cli.c", ("tests/test_headless.py",)),
    ("src/core/try_rt*", ("tests/test_c_try.py", "tests/test_try_using_and_security.py")),
    ("tests/harness/try_*", ("tests/test_c_try.py",)),
    ("tools/gen_abi.py", ("tests/test_gen_abi.py", "tests/test_abi_layouts.py")),
    ("tools/gen_i18n.py", ("tests/test_c_i18n_table.py", "tests/test_c_i18n.py", "tests/test_i18n_hook.py", "tests/test_headless.py")),
    ("lang/*", ("tests/test_lang_packs.py", "tests/test_i18n_hook.py")),
    ("src/tui/kbd_*", ("tests/test_kbd_layout.py",)),
    ("layouts/*", ("tests/test_kbd_layout.py",)),
    ("src/ui/dialog.c", ("tests/test_c_dialog_base.py", "tests/test_pty.py", "tests/test_ui_visual.py", "tests/test_fs_reliability.py")),
    ("src/ui/dialog_confirm.c", ("tests/test_c_dialog_base.py", "tests/test_pty.py", "tests/test_fs_reliability.py")),
    ("src/ui/dialog_input.c", ("tests/test_c_dialog_input.py", "tests/test_c_dialogs_input_keys.py", "tests/test_pty.py", "tests/test_fs_reliability.py")),
    ("src/ui/conflict_dialog.c", ("tests/test_c_conflict_dialog.py", "tests/test_bg_tasks.py", "tests/test_pty.py")),
    ("src/ui/attr_dialogs.c", ("tests/test_c_attr_dialogs.py", "tests/test_trash_undo_attr.py", "tests/test_pty.py")),
    ("src/ui/info_dialog.c", ("tests/test_c_info_dialog.py", "tests/test_trash_undo_attr.py", "tests/test_pty.py")),
    ("src/ui/view_as_dialog.c", ("tests/test_c_view_as_dialog.py", "tests/test_image_viewer.py", "tests/test_pty.py")),
    ("src/ui/bookmark_dlg.c", ("tests/test_c_bookmark_dlg.py", "tests/test_nav_tabs.py", "tests/test_pty.py")),
    ("src/ui/search_dialog.c", ("tests/test_c_search_dialog.py", "tests/test_search_virtual.py", "tests/test_pty.py")),
    ("src/ui/fuzzy_dialog.c", ("tests/test_c_fuzzy_dialog.py", "tests/test_fuzzy_palette.py", "tests/test_pty.py")),
    ("src/ui/user_menu_dlg.c", ("tests/test_c_user_menu_dlg.py", "tests/test_config_usermenu.py", "tests/test_pty.py")),
    ("src/ui/dialogs_input_keys*", ("tests/test_c_dialogs_input_keys.py", "tests/test_pty.py", "tests/test_fs_reliability.py")),
    ("src/ui/dialog_help*", ("tests/test_c_dialog_help.py", "tests/test_help_instruction.py", "tests/test_pty.py")),
    ("src/ui/dialogs_help*", ("tests/test_c_dialog_help.py", "tests/test_help_instruction.py", "tests/test_pty.py")),
    ("src/ui/header_bar*", ("tests/test_c_header_bar.py", "tests/test_ui_visual.py", "tests/test_pty.py")),
    ("src/ui/status_bar*", ("tests/test_c_status_bar.py", "tests/test_ui_visual.py", "tests/test_pty.py")),
    ("src/ui/function_bar*", ("tests/test_c_function_bar.py", "tests/test_ui_visual.py", "tests/test_pty.py")),
    ("src/tui/theme*", ("tests/test_ui_visual.py", "tests/test_pty.py")),
    ("src/tui/screen*", ("tests/test_ui_visual.py", "tests/test_event_loop.py")),
    ("src/tui/input*", ("tests/test_pty.py", "tests/test_kbd_layout.py")),
    ("src/app/app_cmds*", ("tests/test_c_app_cmds.py", "tests/test_pty.py", "tests/test_fs_reliability.py")),
    ("src/app/app_keys*", ("tests/test_c_app_keys.py", "tests/test_pty.py", "tests/test_kbd_layout.py")),
    ("src/app/app_events*", ("tests/test_c_app_events.py", "tests/test_event_loop.py", "tests/test_pty.py")),
    ("src/app/app_loop*", ("tests/test_c_app_loop.py", "tests/test_event_loop.py", "tests/test_pty.py")),
    ("src/app/app.c", ("tests/test_c_app.py", "tests/test_pty.py", "tests/test_event_loop.py")),
    ("src/app/main.c", ("tests/test_c_main.py", "tests/test_headless.py", "tests/test_pty.py")),
    ("src/app/app_open*", ("tests/test_c_app_menu_open.py", "tests/test_pty.py", "tests/test_archive_vfs.py")),
    ("src/app/app_toggles*", ("tests/test_c_app_leaf1.py", "tests/test_pty.py", "tests/test_ui_visual.py")),
    ("src/app/app_ext*", ("tests/test_c_app_ext.py", "tests/test_fuzzy_palette.py", "tests/test_diff_menu_clip.py")),
    ("src/app/app_layout*", ("tests/test_c_app_layout.py", "tests/test_ui_visual.py", "tests/test_pty.py")),
    ("src/app/app_close_dialog*", ("tests/test_c_app_close_dialog.py", "tests/test_pty.py")),
    (".agents/skills/duop-lean-modules/*", ("tests/test_lean_gate.py", "tests/test_lean_split.py")),
    (".agents/skills/duop-reliability-audit/*", ("tests/test_reliability_audit.py", "tests/test_fault_injection.py", "tests/test_audit_live_regs.py", "tests/test_elf_hardening.py")),
    ("tools/abi_verify.c", ("tests/test_reliability_audit.py",)),
    (".agents/hooks/*", ("tests/test_lean_gate.py", "tests/test_i18n_hook.py", "tests/test_c_guards.py")),
    (".claude/hooks/*", ("tests/test_i18n_hook.py", "tests/test_c_guards.py")),
    ("tools/check_c_syntax.py", ("tests/test_c_guards.py",)),
    ("tools/faultlab/audit_null_alloc_c.py", ("tests/test_c_guards.py",)),
    (".githooks/*", ("tests/test_smart_runner.py",)),
)


def _changed_lines(diff_text: str) -> list[str]:
    return [ln for ln in diff_text.splitlines() if ln[:1] in "+-" and not ln.startswith(("+++", "---"))]


def makefile_diff_is_local(diff_text: str) -> bool:
    """True when every changed Makefile line only registers a source file or a test script."""
    lines = _changed_lines(diff_text)
    return bool(lines) and all(REGISTRATION_LINE.match(ln[1:]) for ln in lines)


def header_diff_is_local(diff_text: str) -> bool:
    """True when a duop_*.h change only adds lines (existing declarations are untouched)."""
    lines = _changed_lines(diff_text)
    return bool(lines) and all(ln.startswith("+") for ln in lines)


def local_change(rel: str, staged_only: bool = False) -> bool:
    """Is the working-tree / staged change of a Makefile or duop_*.h file a pure registration / addition?"""
    if rel != "Makefile" and not fnmatch.fnmatch(rel, LOCAL_HEADER_GLOB):
        return False
    cmd = ["git", "diff", "--cached" if staged_only else "HEAD", "-U0", "--", rel]
    res = subprocess.run(cmd, cwd=str(ROOT), capture_output=True, text=True, check=False)
    if res.returncode != 0:
        return False
    return makefile_diff_is_local(res.stdout) if rel == "Makefile" else header_diff_is_local(res.stdout)


def detect_changed_files(staged_only: bool = False) -> list[str]:
    cmds = [["git", "diff", "--cached", "--name-only", "--diff-filter=ACMR"]]
    if not staged_only:
        cmds.append(["git", "diff", "--name-only", "--diff-filter=ACMR"])
        cmds.append(["git", "ls-files", "--others", "--exclude-standard"])
    changed: list[str] = []
    seen: set[str] = set()
    for cmd in cmds:
        res = subprocess.run(cmd, cwd=str(ROOT), capture_output=True, text=True, check=False)
        if res.returncode == 0:
            for line in res.stdout.splitlines():
                p = line.strip()
                if p and p not in seen:
                    seen.add(p)
                    changed.append(p)
    if not changed and not staged_only:
        res = subprocess.run(
            ["git", "diff", "--name-only", "HEAD~1", "HEAD"],
            cwd=str(ROOT),
            capture_output=True,
            text=True,
            check=False,
        )
        if res.returncode == 0:
            changed = [line.strip() for line in res.stdout.splitlines() if line.strip()]
    return changed


def select_tests(changed_files: list[str], local_ok=None) -> tuple[bool, list[str]]:
    """Return (run_full_suite, list_of_test_paths). local_ok(rel) -> True for a Makefile / duop_*.h change that only
    registers or adds (see local_change); such files do not trigger the full suite."""
    selected: list[str] = []
    seen: set[str] = set()
    has_asm = False

    def add_test(t: str) -> None:
        if t not in seen and (ROOT / t).is_file():
            seen.add(t)
            selected.append(t)

    for rel in changed_files:
        if local_ok is not None and local_ok(rel):
            for t in ABI_TESTS if rel.startswith("include/") else ():
                add_test(t)
            continue
        for pat in FULL_SUITE_PATTERNS:
            if fnmatch.fnmatch(rel, pat):
                return True, []
        if rel.endswith(".s") or rel.endswith(".inc"):
            has_asm = True
        if rel.endswith(".c"):
            add_test("tests/test_c_stack_align.py")
        if rel.startswith("tests/test_") and rel.endswith(".py"):
            add_test(rel)
            continue
        matched = False
        for pat, tests in PATH_TEST_RULES:
            if fnmatch.fnmatch(rel, pat):
                matched = True
                for t in tests:
                    add_test(t)
        if not matched and rel.startswith(("src/", "include/")):
            add_test("tests/test_pty.py")

    if has_asm:
        add_test("tests/test_reliability_audit.py")
    return False, selected


def run_selected(tests: list[str], dry_run: bool = False) -> int:
    if dry_run:
        for t in tests:
            print(t)
        return 0
    t0 = time.time()
    if len(tests) >= 2:
        sys.path.insert(0, str(ROOT / "tests"))
        import run_tests as rt  # noqa: E402
        reg = set(rt.registry())
        in_reg = [t for t in tests if t in reg]
        extra = [t for t in tests if t not in reg]
        for t in extra:
            print(f"==> [smart-test] {t}")
            res = subprocess.run([sys.executable, str(ROOT / t)], cwd=str(ROOT), check=False)
            if res.returncode != 0:
                print(f"FAILED: {t} (exit {res.returncode})", file=sys.stderr)
                return res.returncode
        if len(in_reg) >= 2:
            print(f"==> [smart-test] running {len(in_reg)} suite(s) via tests/run_tests.py")
            res = subprocess.run([sys.executable, str(ROOT / "tests/run_tests.py"), "--only", *in_reg], cwd=str(ROOT), check=False)
            if res.returncode != 0:
                return res.returncode
            print(f"SMART TESTS PASSED ({len(tests)} suite(s) in {time.time() - t0:.2f}s).")
            return 0
        tests = in_reg
    for t in tests:
        print(f"==> [smart-test] {t}")
        res = subprocess.run([sys.executable, str(ROOT / t)], cwd=str(ROOT), check=False)
        if res.returncode != 0:
            print(f"FAILED: {t} (exit {res.returncode})", file=sys.stderr)
            return res.returncode
    print(f"SMART TESTS PASSED ({len(tests)} suite(s) in {time.time() - t0:.2f}s).")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description="Run only Duop tests affected by current changes")
    parser.add_argument("--staged", action="store_true", help="Inspect only git-staged files")
    parser.add_argument("--files", nargs="*", help="Explicit list of changed repo-relative files")
    parser.add_argument("--dry-run", action="store_true", help="Print selected tests without running")
    args = parser.parse_args()

    changed = args.files if args.files is not None else detect_changed_files(staged_only=args.staged)
    if not changed:
        print("No changed files detected; running core reliability & PTY smoke tests.")
        return run_selected(["tests/test_reliability_audit.py", "tests/test_pty.py"], dry_run=args.dry_run)

    run_full, tests = select_tests(changed, local_ok=lambda rel: local_change(rel, staged_only=args.staged))
    if run_full:
        if args.dry_run:
            print("FULL_SUITE")
            return 0
        print(f"Core/global changes detected ({len(changed)} file(s)); running full `make -s test`.")
        res = subprocess.run(["make", "-s", "test"], cwd=str(ROOT), check=False)
        return res.returncode

    if not tests:
        print("No code/test files affected (docs/meta only); skipping PTY test suites.")
        return 0
    return run_selected(tests, dry_run=args.dry_run)


if __name__ == "__main__":
    sys.exit(main())
