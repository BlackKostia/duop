# Duop Engineering Rules

Duop (`duop`) is a dual-pane TUI file manager with zero `libc`, direct Linux syscalls and OOP via vtable macros. It uses a hybrid architecture (completed in `v2.6.90`, see `docs/HYBRID_PORTABILITY_PLAN.md`): **per-OS/ISA assembly PAL + SIMD** (`src/os/`, 28 `.s` files across `linux/x86_64` and `linux/aarch64`) beneath **100% portable freestanding C23** (`src/core|tui|fs|ui|app/*.c`, 201 `.c` files; `Makefile` `SRCS` is empty). Both Linux `x86_64` and `aarch64` build full static `duop` binaries. Keep a static ELF with no `libc` and no third-party libraries (hard rule), fast startup, strict modularity (binary size is measured and reported but not gated, and initialised `.data` is allowed), continuous SemVer versioning, and commit-after-verify discipline.

## 1. Strict Modular Layering & Dependencies

Code flows downward only — never introduce upward or cyclic dependencies across layers:

1. **`include/`** (`pal.inc`, `config.inc`, `types.inc`, `oop.inc`, `classes.inc`, `duop_*.h`, and target `include/os/<os>/<arch>/pal_target.inc`): macros, constants, `.struct`/C layouts, and vtable contracts only; emit no instructions or writable data.
2. **`src/os/<os>/<arch>/`** (`pal_*.s` — the only assembly in `src/`): compile-time selected OS/ISA Platform Abstraction Layer (PAL) converting kernel `stat`/`dirent`/`termios`/`sigaction`/`ppoll`/`mremap`/`io_uring`/`pty`/`thread`/SIMD and `_start` (`pal_start.s`) into portable `FileStat` and `os_*` ABI helpers; zero runtime OS branching. **Must never** depend on `src/tui/`, `src/fs/`, `src/ui/`, or `src/app/`.
3. **`src/core/`** (100% C23: `memory.c`, `mem_prims.c`, `memcheck.c`, `string*.c`, `utf8.c`, `hash*.c`, `oop_rt.c`, `syscall.c`, `try_rt.c`, `tmpfile.c`, `config*.c`): runtime, allocator, strings, UTF-8 width, base `Object`/`Widget` vtables, PAL helpers, config. **Must never** contain raw `syscall` instructions or depend on `src/tui/`, `src/fs/`, concrete `src/ui/` widgets, or `src/app/`.
4. **`src/tui/`** (100% C23: `theme.c`, `i18n_*.c`, `screen*.c`, `input*.c`, `kbd_*.c`, `term_caps.c`, `data_dirs.c`, English text in `i18n_en.lang`): terminal TrueColor rendering, themes, built-in English table (generated) + runtime `lang/*.lang` packs and `layouts/*.layout` keyboard maps, SGR mouse/key parser. **Must never** depend on `src/fs/`, concrete `src/ui/` widgets, or `src/app/`.
5. **`src/fs/`** (100% C23: `dir_model*.c`, `dir_sort*.c`, `dir_cache.c`, `dir_watch.c`, `file_ops*.c`, `task*.c`, `archive_*.c`, `pkg_*.c`, `inflate*.c`, `gzip_dec.c`, `zstd_*.c`, `xz_dec.c`, `lzma*.c`, `bzip2_dec.c`, `deflate*.c`, `arc_*.c`, `tar_write.c`, `zip_write.c`, `trash_*.c`, `nav_*.c`, `du_*.c`, `disk_usage.c`, `git_*.c`, `preview_*.c`, `img_*.c`): directory traversal, sorting, virtual panels, async file ops, decoders, package/VFS layer (may query `i18n_get` for error text). **Must never** depend on `Screen_*`, `Input_*`, `theme_*`, concrete UI widgets, or `src/app/`.
6. **`src/ui/`** (100% C23: `status_bar.c`, `header_bar*.c`, `function_bar.c`, `file_pane*.c`, `preview_pane*.c`, `term_pane*.c`, `viewer_*.c`, `hex_edit.c`, `dialog*.c`, `*_dialog.c`, `*_dlg.c`, `popup_*.c`, `syntax.c`, `elf_inspect.c`, `img_*.c`): concrete `Widget`/`Dialog` subclasses rendering exclusively via `ScreenBuffer`, `theme_get`, and `i18n_get`, calling `src/app/` only through `include/duop_app.h`.
7. **`src/app/`** (100% C23: `main.c`, `cli_args.c`, `license.c`, `app.c`, `app_loop.c`, `app_events.c`, `app_keys.c`, `app_layout.c`, `app_cmds.c`, `app_attr.c`, `app_task.c`, `app_open.c`, `app_menu.c`, `app_ext.c`, `app_close_dialog.c`, `app_du.c`, `app_cmdline.c`, `app_choosedir.c`, `app_arc.c`, `app_arc_cmds.c`, `app_pack_cmds.c`, `app_view.c`, `app_toggles.c`): application controller, event loop, command dispatch, CLI arguments.

### Freestanding C23 modules (`src/**/*.c`, registered in Makefile `C_SRCS`)
- Compiler: gcc >= 13 on `x86_64`, `clang --target=…` for `aarch64`/`arm` (Makefile `CFLAGS` is the single source of the ABI flags: `-ffreestanding`, no PIE/CET/unwind tables, `-funsigned-char`, `-fno-strict-aliasing`, `-fno-stack-clash-protection`, `-mno-outline-atomics` on aarch64). Only compiler headers (`<stdint.h>`, `<stddef.h>`, `<stdbool.h>`, `<stdarg.h>`) and `include/duop_abi.h`; **never** libc headers, `asm("syscall")`, VLA, `alloca`, `__int128` or floating point.
- Same layering and PAL boundary as assembly (`check_modularity.py` enforces it for `.c`); kernel details only through `os_*` functions and `FileStat` from `duop_abi.h`; failures are `-errno` values (`os_is_err`), no `errno` global.
- Initialised writable file-scope data (`.data`) is allowed; prefer `const` (`.rodata`) and zero-initialised (`.bss`) where it costs nothing. The ELF stays static: no `libc`, no shared/third-party libraries, no `.dynamic`/`INTERP`/`TEXTREL`, no W+X segment (`audit_elf_hardening.py`). Unaligned parser reads go through `load_le16/32/64`.
- Replacing a `.s` module requires a differential test against a frozen reference of the old code (see `tests/test_c_utf8.py`), and a C symbol keeps the asm calling contract (a pair returned in `rax:rdx` is a 2×`uint64_t` struct). `audit_live_regs.py` must still pass: C clobbers every caller-saved register.
- Budgets (`check_lean.py`): `.c` <= 500 lines, function <= 80 lines, <= 16 non-`static` functions; `.h` <= 400.
- Stack frames above 4608 B (`-Wframe-larger-than`) move their buffers to `mem_alloc`; `tests/test_c_stack_align.py` guards stack alignment across the PAL boundary, and `-errno` results use `intptr_t`.
- Results of `os_*`, `mem_alloc/realloc`, `path_join`, `sys_write_all`, `sys_read_retry` and `tmp_*` are `[[nodiscard]]` under `-Werror`; a discard is a visible `(void)`. A pointer from `mem_alloc` / `*_new` is tested before use (`tools/faultlab/audit_null_alloc_c.py`, PreToolUse C guard). Every `.c` passes `tools/check_c_syntax.py` (project CFLAGS, x86_64 gcc + aarch64 clang).
- All application code is C23; assembly is strictly for the PAL / SIMD (`src/os/`). Frozen reference oracles (`tests/harness/*_reference.s`) remain regression guards; roadmap and migration record: `docs/HYBRID_PORTABILITY_PLAN.md`.

### Anti-Monolith Rule for New Features
- Place cohesive new subsystems in dedicated `.c` files registered in `Makefile` `C_SRCS` (or `PAL_SRCS` for OS/arch backends) rather than growing `src/ui/dialog.c`, `src/app/app.c`, or `src/ui/file_pane.c`.
- Put compile-time OS syscall numbers, kernel structs, and `OS_*` inline macros in `include/os/<os>/<arch>/pal_target.inc` and normalized ABI adapters (`os_statat`, `os_dir_read`/`os_dir_next`, `os_mem_remap`, `os_poll_in`, terminal/signal adapters) in `src/os/<os>/<arch>/pal_os.s`; never add runtime OS checks (`cmp [g_os]`) or function-pointer tables.

### Lean Modules (`duop-lean-modules`, enforced by hooks, `verify_build.sh` and pre-commit)
- Budgets: `src/**/*.s` ≤ 500 lines (aim 150–300), top-level routine ≤ 80 lines, ≤ 16 `.global` symbols; `include/**/*.inc` ≤ 400 lines; `tests/*.py` ≤ 500 lines, `def` ≤ 100 lines.
- Legacy offenders are frozen in `.agents/skills/duop-lean-modules/scripts/lean_baseline.json` and may only shrink; after shrinking run `check_lean.py --update`.
- Grow by splitting: new concern → new `src/<layer>/<topic>.s`; long routine → named local helpers.
- Mechanical, semantics-preserving splitters live in the skill (`split_routine.py` → `Routine__step` frame-sharing steps, `split_module.py` → sibling modules, `check_lean.py --migrate`); hot inner loops and big state machines are redesigned by hand.

## 2. Assembly, ABI & I18n Invariants

- Use `.intel_syntax noprefix` at the top of every x86-64 `.s`/`.inc` file (AArch64 `.s` use GNU ARM syntax).
- Immutable data in `.section .rodata`, zero-initialised globals in `.section .bss` (or heap `mem_alloc`); an initialised `.section .data` is allowed when it is simpler than an `init` routine.
- Follow System V AMD64 ABI (`rdi` = `this`, preserve `rbx`, `rbp`, `r12`–`r15`, 16-byte stack alignment across `call`).
- Every user-visible string MUST be a `LangStrings` slot (`include/types.inc`) with its English text in `src/tui/i18n_en.lang` (compiled by `tools/gen_i18n.py` into `build/i18n_en.c`; the key table comes from `include/types.inc`) and its Ukrainian translation in `lang/uk.lang`; other interface languages are `lang/*.lang` packs and keyboard layouts are `layouts/*.layout` files — never compile translations or layout codepoints into code. Compute layout columns via `utf8_display_width`.

## 3. Verification, SemVer & Git Commits

- Use workspace skills in `.agents/skills/`:
  - `duop-tdd-first` to design and write failing tests in `tests/` (happy-path + edge/fault cases) **before** implementing changes in `src/` or `include/`
  - `duop-asm-to-c-migration` when replacing a `.s` module by C (oracle, differential harness, mutation checks, gates)
  - `duop-modular-asm` when adding/splitting modules, OOP classes, or registering targets (C by default)
  - `duop-lean-modules` before creating/growing any module, on `LEAN:` errors, and when refactoring large files/routines
  - `duop-multiplatform-pal` when touching OS syscalls, `pal.inc`, `FileStat`, `src/os/`, `Makefile` (`OS`/`ARCH`), or SIMD code
  - `duop-ui-i18n-widget` when building UI widgets, themes, mouse zones, or translations
  - `duop-pty-verify` to validate modularity, static-ELF audit (no libc/dynamic/W+X), release/debug builds, and PTY tests
  - `duop-version-release` to manage `VERSION`, `ROADMAP.md`, Conventional Commits, and Git tags
- Never consider code changes complete until `.agents/skills/duop-pty-verify/scripts/verify_build.sh` passes cleanly.
- `make test` runs the registry (the `test-serial:` recipe of the Makefile, one `python3 tests/test_x.py` line per suite) in parallel through `tests/run_tests.py` (~1 min); `DUOP_PTY_FULL_WAIT=1` restores fixed PTY waits when a timing failure needs debugging.
- Hooks are tiered: `pre-commit` runs lint/audit/C gates and, for module-local commits (Makefile registration lines, additive `include/duop_*.h` declarations, one module + its tests), only the smart tests of `tests/run_smart_tests.py`; changes to `src/core`, `src/os`, `include/*.inc`, existing declarations, `src/app/{main,app,app_loop,app_events}.c` run the full `make test`. `pre-push` always runs the complete `make test` plus faultlab sweeps, SEU and fuzzing. Do not edit tracked files while a commit hook is running.
- Whenever modifying `src/` or `include/`, ensure `VERSION` is ahead of any existing release tag `vX.Y.Z` (bump via `bump_version.py`), rebuild so `build/version.inc` matches, verify with PTY tests, and commit with a Conventional Commit (`feat|fix|refactor|perf|build|docs|test|chore`).
