# Duop — Hybrid Assembly PAL + Freestanding C23 Makefile (compile-time OS & ARCH selection)

OS      ?= linux
ARCH    ?= x86_64
JOBS    ?= $(shell nproc 2>/dev/null || echo 4)
MEMCHECK ?= 0
TARGET  ?= duop
BUILD   := build
VERSION := $(strip $(shell cat VERSION))
PAL_INC := include/os/$(OS)/$(ARCH)

AS      := as
LD      := ld
CC      := gcc
CTARGET :=
ifeq ($(ARCH),aarch64)
AS      := aarch64-linux-gnu-as
LD      := aarch64-linux-gnu-ld
CC      := clang
CTARGET := --target=aarch64-linux-gnu -mno-outline-atomics -Wno-unused-command-line-argument
endif

ifneq ($(ARCH),x86_64)
BUILD   := build_$(ARCH)
endif

ifeq ($(MEMCHECK),1)
TARGET  := duop_memcheck
BUILD   := build_mc
endif

ASFLAGS := -I $(PAL_INC) -I include -I $(BUILD) -g --strip-local-absolute
LDFLAGS := -z noexecstack --gc-sections
# Freestanding C23 (docs/HYBRID_PORTABILITY_PLAN.md "ABI-прапорці збірки"); no libc, no PIE/CET/unwind tables
CFLAGS  := $(CTARGET) -std=gnu2x -Os -ffreestanding -fno-pic -fno-pie \
	-fcf-protection=none -fno-stack-protector -fno-asynchronous-unwind-tables -fno-unwind-tables -fno-ident \
	-funsigned-char -fno-strict-aliasing -fno-common -ffunction-sections -fdata-sections \
	-g -Wall -Wextra -Werror -Wframe-larger-than=4608 -I include -I $(BUILD)
ifneq ($(ARCH),aarch64)
CFLAGS  += -fno-stack-clash-protection
endif

ifeq ($(MEMCHECK),1)
ASFLAGS += --defsym MEMCHECK=1
CFLAGS  += -DMEMCHECK=1
endif

VER_INC := $(BUILD)/version.inc

# Legacy non-PAL assembly list (empty: 100% of core/tui/fs/ui/app is freestanding C23; assembly is PAL-only under src/os/)
SRCS :=

# Freestanding C23 modules (docs/HYBRID_PORTABILITY_PLAN.md); migrated leaf-first from the .s list above
C_SRCS := \
	src/core/utf8.c \
	src/core/hash.c \
	src/core/hash_xxh.c \
	src/core/hash_crc64.c \
	src/core/string.c \
	src/core/string_path.c \
	src/core/string_format.c \
	src/core/string_datetime.c \
	src/core/memory.c \
	src/core/oop_rt.c \
	src/core/tmpfile.c \
	src/core/hash_file.c \
	src/core/config.c \
	src/core/config_parse.c \
	src/core/config_load.c \
	src/core/config_expand.c \
	src/core/syscall.c \
	src/core/try_rt.c \
	src/core/mem_prims.c \
	src/core/memcheck.c \
	src/fs/inflate.c \
	src/fs/inflate_stream.c \
	src/fs/gzip_dec.c \
	src/fs/bzip2_dec.c \
	src/fs/lzma_dec.c \
	src/fs/lzma2_dec.c \
	src/fs/xz_dec.c \
	src/fs/deflate.c \
	src/fs/deflate_huff.c \
	src/fs/zstd_fse.c \
	src/fs/zstd_huf.c \
	src/fs/zstd_seq.c \
	src/fs/zstd_dec.c \
	src/fs/arc_index.c \
	src/fs/arc_tar.c \
	src/fs/arc_zip.c \
	src/fs/arc_source.c \
	src/fs/arc_detect.c \
	src/fs/arc_extract.c \
	src/fs/arc_path.c \
	src/fs/arc_unpack_item.c \
	src/fs/arc_unpack.c \
	src/fs/arc_task.c \
	src/tui/theme.c \
	src/tui/kbd_map.c \
	src/tui/kbd_layout.c \
	src/tui/data_dirs.c \
	src/tui/screen_rect.c \
	src/tui/term_caps.c \
	src/tui/input.c \
	src/tui/input_parse.c \
	src/tui/input_seq.c \
	src/tui/i18n_env.c \
	src/tui/i18n_pack.c \
	src/tui/i18n_keys.c \
	src/tui/screen.c \
	src/tui/screen_buffer.c \
	src/tui/screen_text.c \
	src/fs/img_bmp.c \
	src/fs/img_tga.c \
	src/fs/img_pnm.c \
	src/fs/img_gif.c \
	src/fs/img_png.c \
	src/fs/img_jpeg.c \
	src/fs/img_kitty.c \
	src/fs/img_quant.c \
	src/fs/img_sixel.c \
	src/fs/dir_diff.c \
	src/fs/dir_search_match.c \
	src/fs/dir_search.c \
	src/fs/dir_fuzzy.c \
	src/fs/dir_fuzzy_walk.c \
	src/fs/fs_write.c \
	src/fs/du_links.c \
	src/fs/du_cache.c \
	src/fs/du_parallel.c \
	src/fs/disk_usage.c \
	src/fs/trash_info.c \
	src/fs/trash_dirs.c \
	src/fs/trash_undo.c \
	src/fs/nav_state.c \
	src/fs/nav_persist.c \
	src/fs/git_trust.c \
	src/fs/dir_watch.c \
	src/fs/file_ops_overwrite.c \
	src/fs/pkg_fetch.c \
	src/fs/dir_cache.c \
	src/fs/git_status.c \
	src/fs/preview_io.c \
	src/fs/preview_helper.c \
	src/fs/img_core.c \
	src/fs/dir_sort.c \
	src/fs/dir_sort_radix.c \
	src/fs/dir_model_stat.c \
	src/fs/dir_model_select.c \
	src/fs/dir_model.c \
	src/fs/dir_model_filter.c \
	src/fs/dir_model_load.c \
	src/fs/dir_model_stream.c \
	src/fs/file_ops.c \
	src/fs/file_ops_copy.c \
	src/fs/file_ops_delete.c \
	src/fs/task.c \
	src/fs/task_worker.c \
	src/fs/task_control.c \
	src/fs/task_conflict.c \
	src/fs/pkg_verify.c \
	src/fs/pkg_registry.c \
	src/fs/archive_parse.c \
	src/fs/archive_vfs.c \
	src/fs/pkg_archive.c \
	src/fs/pkg_manager.c \
	src/fs/pkg_cli.c \
	src/fs/pkg_actions.c \
	src/fs/pkg_vfs.c \
	src/fs/pkg_vfs_pane.c \
	src/fs/pkg_popup.c \
	src/fs/pkg_preview.c \
	src/ui/status_bar.c \
	src/ui/popup_osc52.c \
	src/ui/term_keys.c \
	src/ui/term_pane_input.c \
	src/ui/preview_pane_mouse.c \
	src/ui/header_bar_git.c \
	src/ui/file_pane_layout.c \
	src/ui/syntax.c \
	src/ui/elf_inspect.c \
	src/ui/dialogs_input_keys.c \
	src/ui/git_render.c \
	src/ui/du_render.c \
	src/ui/img_overlay.c \
	src/ui/img_preview.c \
	src/ui/img_quad.c \
	src/ui/img_braille.c \
	src/ui/img_preview_api.c \
	src/ui/viewer_scrollbar.c \
	src/ui/viewer_zoom.c \
	src/ui/viewer_zoom_rect.c \
	src/ui/dialog.c \
	src/ui/dialog_confirm.c \
	src/ui/dialog_input.c \
	src/ui/conflict_dialog.c \
	src/ui/attr_dialogs.c \
	src/ui/info_dialog.c \
	src/ui/view_as_dialog.c \
	src/ui/bookmark_dlg.c \
	src/ui/search_dialog.c \
	src/ui/fuzzy_dialog.c \
	src/ui/user_menu_dlg.c \
	src/ui/bulk_rename.c \
	src/ui/progress_dialog.c \
	src/ui/du_progress_dlg.c \
	src/ui/dialog_help.c \
	src/ui/pkg_dialog.c \
	src/ui/viewer_pixels.c \
	src/ui/viewer_image.c \
	src/ui/hex_edit.c \
	src/ui/viewer_dialog.c \
	src/ui/viewer_dialog_render.c \
	src/ui/preview_mode.c \
	src/ui/preview_pane.c \
	src/ui/preview_pane_render.c \
	src/ui/term_pane.c \
	src/ui/function_bar.c \
	src/ui/header_bar.c \
	src/ui/file_pane_tabs.c \
	src/ui/file_pane_keys.c \
	src/ui/file_pane_mouse.c \
	src/ui/popup_menu.c \
	src/ui/file_pane_rows.c \
	src/ui/file_pane_render.c \
	src/ui/file_pane.c \
	src/app/app_choosedir.c \
	src/app/app_arc.c \
	src/app/app_arc_cmds.c \
	src/app/app_view.c \
	src/app/app_toggles.c \
	src/app/app_du.c \
	src/app/app_cmdline.c \
	src/app/app_task.c \
	src/app/app_menu.c \
	src/app/app_open.c \
	src/app/app_cmds.c \
	src/app/app_attr.c \
	src/app/app_ext.c \
	src/app/app_close_dialog.c \
	src/app/app_layout.c \
	src/app/app_keys.c \
	src/app/app_events.c \
	src/app/app_loop.c \
	src/app/app.c \
	src/app/cli_args.c \
	src/app/license.c \
	src/app/main.c \
	src/fs/file_ops_read.c

PAL_SRCS := \
	src/os/$(OS)/$(ARCH)/pal_os.s \
	src/os/$(OS)/$(ARCH)/pal_fault.s \
	src/os/$(OS)/$(ARCH)/pal_uring.s \
	src/os/$(OS)/$(ARCH)/pal_simd.s \
	src/os/$(OS)/$(ARCH)/pal_pty.s \
	src/os/$(OS)/$(ARCH)/pal_thread.s \
	src/os/$(OS)/$(ARCH)/pal_du.s \
	src/os/$(OS)/$(ARCH)/pal_time.s \
	src/os/$(OS)/$(ARCH)/pal_file.s \
	src/os/$(OS)/$(ARCH)/pal_sys_fd.s \
	src/os/$(OS)/$(ARCH)/pal_sys_path.s \
	src/os/$(OS)/$(ARCH)/pal_sys_proc.s \
	src/os/$(OS)/$(ARCH)/pal_sys_mem.s

# Process entry (_start -> duop_main -> os_exit) for full, headless, and PAL selftest builds
PAL_START_SRCS := \
	src/os/$(OS)/$(ARCH)/pal_start.s

# C generated at build time into $(BUILD) (tools/gen_i18n.py: the English table + key names from src/tui/i18n_en.lang)
GEN_OBJS := $(BUILD)/i18n_en.o

OBJS := $(patsubst src/%.s,$(BUILD)/%.o,$(SRCS) $(PAL_SRCS) $(PAL_START_SRCS)) $(patsubst src/%.c,$(BUILD)/%.o,$(C_SRCS)) $(GEN_OBJS)
INCS := $(wildcard include/*.inc) $(wildcard $(PAL_INC)/*.inc)

.PHONY: all release run test test-serial memcheck verify hooks symbols symbols-check clean pal-selftest check-toolchain headless

all: $(TARGET)

# Stripped release build: a clean tree first, then a parallel build (a plain `release: clean $(TARGET)` cannot be run with -j: clean would race the compiler)
release:
	@$(MAKE) --no-print-directory clean
	@$(MAKE) --no-print-directory -j$(JOBS) ASFLAGS="-I $(PAL_INC) -I include -I $(BUILD)" LDFLAGS="-s -z noexecstack --gc-sections" $(TARGET)

$(TARGET): $(OBJS)
	$(LD) $(LDFLAGS) $(OBJS) -o $@

# Unstripped link of the same objects: the binary-level audits (audit_live_regs.py) need the symbol table
$(BUILD)/duop.sym: $(OBJS)
	$(LD) -z noexecstack --gc-sections $(OBJS) -o $@

$(VER_INC): VERSION
	@mkdir -p $(BUILD)
	@printf '.macro DUOP_VER_ASCII\n    .ascii "$(VERSION)"\n.endm\n.macro DUOP_VER_ASCIZ\n    .asciz "$(VERSION)"\n.endm\n' > $@

$(BUILD)/version.h: VERSION
	@mkdir -p $(BUILD)
	@printf '#pragma once\n#define DUOP_VERSION "$(VERSION)"\n' > $@

$(BUILD)/%.o: src/%.s $(INCS) $(VER_INC)
	@mkdir -p $(dir $@)
	$(AS) $(ASFLAGS) $< -o $@

$(BUILD)/%.o: src/%.c $(wildcard include/*.h) | $(BUILD)/version.h $(BUILD)/i18n_slots.h $(BUILD)/duop_classes.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

# LS_<key> slot indices of LangStrings (i18n_get()->s[LS_cli_usage]), generated from include/types.inc
$(BUILD)/i18n_slots.h: tools/gen_i18n.py include/types.inc
	@mkdir -p $(BUILD)
	python3 tools/gen_i18n.py --out-slots $@

# C structs of the assembler's classes and .struct layouts (tools/gen_abi.py; hand-written duop_*.h structs are skipped)
$(BUILD)/duop_classes.h: tools/gen_abi.py include/classes.inc include/dialogs_ext.inc include/types.inc include/types_core.inc include/pal.inc include/config.inc include/oop.inc $(wildcard include/duop_*.h)
	@mkdir -p $(BUILD)
	python3 tools/gen_abi.py --out $@

$(BUILD)/i18n_en.c: tools/gen_i18n.py src/tui/i18n_en.lang include/types.inc
	@mkdir -p $(BUILD)
	python3 tools/gen_i18n.py --out $@

$(BUILD)/i18n_en.o: $(BUILD)/i18n_en.c $(wildcard include/*.h) | $(BUILD)/version.h $(BUILD)/i18n_slots.h
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

# Headless build (docs/HYBRID_PORTABILITY_PLAN.md, M1): the command line (--help / --version / --license) without the TUI. It links the PAL,
# pal_start and every portable C module of the selected ARCH, so it runs the C23 layers natively (x86_64) or under qemu-aarch64. Its own
# objects live in $(BUILD)_headless/ so tests that glob $(BUILD)/**/*.o never see a second entry point.
HB            := $(BUILD)_headless
HEADLESS_C    := src/app/main_headless.c
HEADLESS_OBJS := $(patsubst src/%.s,$(HB)/%.o,$(PAL_START_SRCS)) $(patsubst src/%.c,$(HB)/%.o,$(HEADLESS_C)) \
	$(patsubst src/%.s,$(BUILD)/%.o,$(PAL_SRCS)) $(patsubst src/%.c,$(BUILD)/%.o,$(filter-out src/app/main.c,$(C_SRCS))) $(GEN_OBJS)

headless: $(BUILD)/duop_headless

$(BUILD)/duop_headless: $(HEADLESS_OBJS)
	$(LD) $(LDFLAGS) $(HEADLESS_OBJS) -o $@

$(HB)/%.o: src/%.s $(INCS)
	@mkdir -p $(dir $@)
	$(AS) $(ASFLAGS) $< -o $@

$(HB)/%.o: src/%.c $(wildcard include/*.h) | $(BUILD)/version.h $(BUILD)/i18n_slots.h $(BUILD)/duop_classes.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

# PAL walking skeleton: C self-test linked with the PAL of the selected ARCH (qemu-aarch64 runs the arm64 one).
# Objects live in build_selftest/<arch>/ so they never leak into the main build/ tree that tests glob.
ST      := build_selftest/$(ARCH)
ST_OBJS := $(patsubst src/%.s,$(ST)/%.o,$(PAL_SRCS) $(PAL_START_SRCS)) $(ST)/pal_selftest/pal_selftest.o $(ST)/pal_selftest/mem.o
SELFTEST := $(ST)/selftest

$(ST)/%.o: src/%.s $(INCS)
	@mkdir -p $(dir $@)
	$(AS) $(ASFLAGS) $< -o $@

$(ST)/pal_selftest/%.o: tools/pal_selftest/%.c include/duop_abi.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(SELFTEST): $(ST_OBJS)
	$(LD) $(LDFLAGS) $(ST_OBJS) -o $@

pal-selftest: $(SELFTEST)
ifeq ($(ARCH),aarch64)
	qemu-aarch64 $(SELFTEST)
else
	$(SELFTEST)
endif

check-toolchain:
	@$(CC) --version | head -1
	@$(AS) --version | head -1
	@$(LD) --version | head -1

run: $(TARGET)
	./$(TARGET)

# The registry of test scripts is the recipe of test-serial (one `python3 tests/test_x.py` line each: hooks and tests/run_smart_tests.py
# recognise those lines); `make test` runs the same scripts in parallel through tests/run_tests.py.
test: $(TARGET)
	python3 tests/run_tests.py

test-serial: $(TARGET)
	python3 tests/test_pal_architecture.py
	python3 tests/test_abi_cross.py
	python3 tests/test_pal_parity.py
	python3 tests/test_pal_selftest.py
	python3 tests/test_c_utf8.py
	python3 tests/test_c_hash.py
	python3 tests/test_c_inflate.py
	python3 tests/test_c_inflate_stream.py
	python3 tests/test_c_gzip_dec.py
	python3 tests/test_c_zstd_dec.py
	python3 tests/test_c_xz_dec.py
	python3 tests/test_c_bzip2_dec.py
	python3 tests/test_c_deflate.py
	python3 tests/test_c_arc_zstd.py
	python3 tests/test_c_arc_xz.py
	python3 tests/test_c_arc_bzip2.py
	python3 tests/test_c_arc_index.py
	python3 tests/test_c_arc_tar.py
	python3 tests/test_c_arc_zip.py
	python3 tests/test_c_arc_vfs.py
	python3 tests/test_c_arc_detect.py
	python3 tests/test_c_arc_unpack.py
	python3 tests/test_arc_unpack_ui.py
	python3 tests/test_arc_enter_ui.py
	python3 tests/test_c_string.py
	python3 tests/test_c_syscall.py
	python3 tests/test_c_fsmeta.py
	python3 tests/test_c_try.py
	python3 tests/test_c_memory.py
	python3 tests/test_c_memcheck.py
	python3 tests/test_c_core_files.py
	python3 tests/test_c_config_expand.py
	python3 tests/test_c_config.py
	python3 tests/test_c_tui_data.py
	python3 tests/test_c_input.py
	python3 tests/test_c_i18n.py
	python3 tests/test_c_i18n_table.py
	python3 tests/test_c_app_api.py
	python3 tests/test_c_harness_utils.py
	python3 tests/test_headless.py
	python3 tests/test_run_tests.py
	python3 tests/test_c_screen.py
	python3 tests/test_c_img.py
	python3 tests/test_c_img_out.py
	python3 tests/test_c_fs_small.py
	python3 tests/test_c_dir_search.py
	python3 tests/test_c_dir_fuzzy.py
	python3 tests/test_c_du_tables.py
	python3 tests/test_c_du_walk.py
	python3 tests/test_c_du_populate.py
	python3 tests/test_c_trash.py
	python3 tests/test_c_nav_state.py
	python3 tests/test_c_git_trust.py
	python3 tests/test_c_dir_watch.py
	python3 tests/test_c_fops_small.py
	python3 tests/test_c_fops_overwrite.py
	python3 tests/test_c_pkg_fetch.py
	python3 tests/test_c_dir_cache.py
	python3 tests/test_c_git_status.py
	python3 tests/test_c_preview.py
	python3 tests/test_c_img_core.py
	python3 tests/test_c_dir_sort.py
	python3 tests/test_c_dir_model_stat.py
	python3 tests/test_c_dir_model.py
	python3 tests/test_c_dir_model_filter.py
	python3 tests/test_c_dir_model_load.py
	python3 tests/test_c_file_ops.py
	python3 tests/test_c_file_ops_copy.py
	python3 tests/test_c_file_ops_delete.py
	python3 tests/test_c_task.py
	python3 tests/test_c_task_control.py
	python3 tests/test_c_task_conflict.py
	python3 tests/test_c_pkg_verify.py
	python3 tests/test_c_pkg_registry.py
	python3 tests/test_c_archive.py
	python3 tests/test_c_pkg_archive.py
	python3 tests/test_c_pkg_manager.py
	python3 tests/test_c_pkg_cli.py
	python3 tests/test_c_pkg_actions.py
	python3 tests/test_c_pkg_vfs.py
	python3 tests/test_c_pkg_vfs_pane.py
	python3 tests/test_c_pkg_popup.py
	python3 tests/test_c_status_bar.py
	python3 tests/test_c_ui_leaf1.py
	python3 tests/test_c_file_pane_layout.py
	python3 tests/test_c_syntax.py
	python3 tests/test_c_elf_inspect.py
	python3 tests/test_c_dialogs_input_keys.py
	python3 tests/test_c_git_du_render.py
	python3 tests/test_c_img_overlay.py
	python3 tests/test_c_img_cells.py
	python3 tests/test_c_img_preview_api.py
	python3 tests/test_c_viewer_scrollbar.py
	python3 tests/test_c_viewer_zoom.py
	python3 tests/test_c_dialog_base.py
	python3 tests/test_c_dialog_input.py
	python3 tests/test_c_conflict_dialog.py
	python3 tests/test_c_attr_dialogs.py
	python3 tests/test_c_info_dialog.py
	python3 tests/test_c_view_as_dialog.py
	python3 tests/test_c_bookmark_dlg.py
	python3 tests/test_c_search_dialog.py
	python3 tests/test_c_fuzzy_dialog.py
	python3 tests/test_c_user_menu_dlg.py
	python3 tests/test_c_bulk_rename.py
	python3 tests/test_c_progress_dialog.py
	python3 tests/test_c_du_progress_dlg.py
	python3 tests/test_c_dialog_help.py
	python3 tests/test_c_pkg_dialog.py
	python3 tests/test_c_viewer_pixels.py
	python3 tests/test_c_viewer_image.py
	python3 tests/test_c_hex_edit.py
	python3 tests/test_c_viewer_dialog.py
	python3 tests/test_c_preview_mode.py
	python3 tests/test_c_preview_pane.py
	python3 tests/test_c_term_pane.py
	python3 tests/test_c_function_bar.py
	python3 tests/test_c_header_bar.py
	python3 tests/test_c_file_pane_tabs.py
	python3 tests/test_c_file_pane_keys.py
	python3 tests/test_c_file_pane_mouse.py
	python3 tests/test_c_popup_menu.py
	python3 tests/test_c_file_pane_render.py
	python3 tests/test_c_file_pane.py
	python3 tests/test_c_app_leaf1.py
	python3 tests/test_c_app_du_cmd.py
	python3 tests/test_c_app_task.py
	python3 tests/test_c_app_menu_open.py
	python3 tests/test_c_app_cmds.py
	python3 tests/test_c_app_attr.py
	python3 tests/test_c_app_ext.py
	python3 tests/test_c_app_close_dialog.py
	python3 tests/test_c_app_layout.py
	python3 tests/test_c_app_keys.py
	python3 tests/test_c_app_events.py
	python3 tests/test_c_app_loop.py
	python3 tests/test_c_app.py
	python3 tests/test_c_main.py
	python3 tests/test_aarch64_full.py
	python3 tests/test_c_stack_align.py
	python3 tests/test_c_guards.py
	python3 tests/test_abi_layouts.py
	python3 tests/test_gen_abi.py
	python3 tests/test_c_ui_oop.py
	python3 tests/test_pty.py
	python3 tests/test_fs_reliability.py
	python3 tests/test_performance_dir.py
	python3 tests/test_bg_tasks.py
	python3 tests/test_fastpath.py
	python3 tests/test_choosedir.py
	python3 tests/test_config_usermenu.py
	python3 tests/test_nav_tabs.py
	python3 tests/test_search_virtual.py
	python3 tests/test_trash_undo_attr.py
	python3 tests/test_fuzzy_palette.py
	python3 tests/test_img_preview.py
	python3 tests/test_image_formats.py
	python3 tests/test_image_quality.py
	python3 tests/test_security_reliability.py
	python3 tests/test_disk_usage.py
	python3 tests/test_du_progress.py
	python3 tests/test_du_sizes.py
	python3 tests/test_archive_vfs.py
	python3 tests/test_arc_readonly.py
	python3 tests/test_arc_fuzz.py
	python3 tests/test_git_status.py
	python3 tests/test_bulk_rename.py
	python3 tests/test_viewer_pro.py
	python3 tests/test_diff_menu_clip.py
	python3 tests/test_term_pane.py
	python3 tests/test_nav_viewer.py
	python3 tests/test_event_loop.py
	python3 tests/test_kbd_layout.py
	python3 tests/test_help_instruction.py
	python3 tests/test_lang_packs.py
	python3 tests/test_ui_visual.py
	python3 tests/test_pkg_manager.py
	python3 tests/test_pkg_extensions.py
	python3 tests/test_pkg_improvements.py
	python3 tests/test_reliability_audit.py
	python3 tests/test_fault_injection.py
	python3 tests/test_smart_runner.py
	python3 tests/test_try_using_and_security.py
	python3 tests/test_security_fixes.py
	python3 tests/test_perf_and_arch_fixes.py
	python3 tests/test_critical_fixes.py
	python3 tests/test_medium_fixes.py
	python3 tests/test_audit_regressions_parsers.py
	python3 tests/test_audit_regressions_mem.py
	python3 tests/test_audit_regressions_io.py
	python3 tests/test_audit_regressions_sig.py
	python3 tests/test_crash_regressions.py
	python3 tests/test_archive_hardening.py
	python3 tests/test_cmd_expand_safety.py
	python3 tests/test_git_trust.py
	python3 tests/test_trash_spec.py
	python3 tests/test_copy_overwrite_atomic.py
	python3 tests/test_config_limits.py
	python3 tests/test_decoder_bounds.py
	python3 tests/test_elf_hardening.py
	python3 tests/test_pane_scrollbar.py
	python3 tests/test_audit_live_regs.py

test-smart: $(TARGET)
	python3 tests/run_smart_tests.py

audit-all: $(TARGET) $(BUILD)/duop.sym
	python3 .agents/skills/duop-reliability-audit/scripts/audit_stack_abi.py
	python3 .agents/skills/duop-reliability-audit/scripts/audit_cfg_stack.py
	python3 .agents/skills/duop-reliability-audit/scripts/audit_syscall_clobber.py
	python3 .agents/skills/duop-reliability-audit/scripts/audit_abi_regs.py
	python3 .agents/skills/duop-reliability-audit/scripts/audit_oop_vtables.py
	python3 .agents/skills/duop-reliability-audit/scripts/audit_fd_eintr.py
	python3 .agents/skills/duop-reliability-audit/scripts/audit_elf_hardening.py $(TARGET)
	python3 .agents/skills/duop-reliability-audit/scripts/audit_live_regs.py $(BUILD)/duop.sym
	python3 .agents/skills/duop-docs-sync/scripts/gen_symbols_catalog.py --check
	gcc -O2 -Wall -Wextra tools/abi_verify.c -o $(BUILD)/abi_verify && $(BUILD)/abi_verify include/os/linux/x86_64/pal_target.inc

symbols:
	python3 .agents/skills/duop-docs-sync/scripts/gen_symbols_catalog.py

symbols-check:
	python3 .agents/skills/duop-docs-sync/scripts/gen_symbols_catalog.py --check

memcheck:
	$(MAKE) MEMCHECK=1
	python3 tests/test_memcheck.py

verify:
	./.agents/skills/duop-pty-verify/scripts/verify_build.sh

hooks:
	git config core.hooksPath .githooks
	chmod +x .githooks/* .agents/hooks/*.py .agents/skills/*/scripts/*

-include $(shell find $(BUILD) $(HB) $(ST) -name '*.d' 2>/dev/null)

clean:
	rm -rf $(BUILD) $(HB) $(TARGET) build_mc build_aarch64 build_aarch64_headless build_headless build_selftest duop_memcheck
