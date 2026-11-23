# Duop (`duop`)

[![CI](https://github.com/BlackKostia/duop/actions/workflows/ci.yml/badge.svg)](https://github.com/BlackKostia/duop/actions/workflows/ci.yml)
[![Release](https://img.shields.io/github/v/release/BlackKostia/duop?label=release)](https://github.com/BlackKostia/duop/releases/latest)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![Static ELF](https://img.shields.io/badge/static_ELF-187_KiB_(x86__64)_%7C_209_KiB_(aarch64)-success)](#benchmarks)

**English** | [Українська](README.uk.md)

**A ~190 KiB zero-`libc` dual-pane terminal file manager for Linux (`x86_64` & `aarch64`).**
One static binary — no `libc`, no `ncurses`, no runtime dependencies. Opens a 4.8-million-file directory in **26 ms** (15.4 MiB RSS) and packs fuzzy search, pixel-exact image preview, git badges, hex/ELF inspection, archive VFS, and an embedded PTY terminal out of the box.

![Duop dual-pane view (Tokyo Night theme)](docs/media/main.svg)

<details>
<summary><strong>More screenshots: Live Preview, Command Palette, Catppuccin Theme, Ukrainian UI</strong></summary>

#### Live Code Preview (`F9`)
![Duop live preview](docs/media/preview.svg)

#### Command Palette (`Ctrl+K`)
![Duop command palette](docs/media/palette.svg)

#### Catppuccin Theme (`Alt+T`) & Ukrainian Localization (`Alt+L`)
| Catppuccin Theme | Український інтерфейс (`--lang ua`) |
| --- | --- |
| ![Catppuccin](docs/media/theme.svg) | ![Ukrainian UI](docs/media/ukrainian.svg) |

*(All screenshots above are generated deterministically from a real PTY session via [`tools/screenshot_svg.py`](tools/screenshot_svg.py).)*
</details>

---

## Why Duop?

- **Drop-in anywhere (Containers, Servers, Rescue environments)**: single static ELF64 (`191,528` B / `187 KiB` on `x86_64`, `214,128` B / `209 KiB` on `aarch64`, `.data = 0`, NX-safe). Copy it with `scp` or `docker cp` into a minimal container or embedded board and it runs immediately.
- **Built for extreme directories**:
  - Compact 32-byte `FileEntry` + contiguous `name_pool` + 4-byte `order[]` index sorting (Introsort + LSD Radix Sort).
  - Lazy `d_type` open + rolling `io_uring` (`IORING_OP_STATX`) batch statting + `copy_file_range` / `FICLONE` fast-path copy.
  - Non-blocking background tasks (`fork` + `MAP_SHARED` atomic progress, Pause/Resume/Background, Conflict resolution).
  - Event-driven main loop (`ppoll` with infinite idle timeout — **0 wake-ups when idle**, `inotify` live refresh, debounced async preview, `DEC 2026` synchronized frames).
- **Rich TUI & Developer Tooling**:
  - TrueColor RGB diff-renderer, 3 built-in themes (`Tokyo Night`, `Catppuccin`, `Emerald`), live EN/UA localization (`--lang en|ua`, `Alt+L`), SGR-1006 mouse & drag-and-drop.
  - Command Palette (`Ctrl+K`), Recursive Fuzzy Finder (`Alt+P`), Recursive Name/Grep Virtual Panel (`Ctrl+F`), Bookmarks (`Ctrl+D`), Frecency (`z`), Tabs (`Ctrl+T`/`Ctrl+W`), History (`Alt+Left`/`Alt+Right`), Package Manager (`Alt+K`) & User Menu (`Alt+U`).
  - Archive VFS (`.tar`, `.tar.gz`, `.zip`), Git branch/status badges, Disk Usage analyzer (`Alt+D`), Bulk Rename (`Alt+B`), Dir Diff (`Alt+=`), Right-click Context Menu + `OSC 52` clipboard.
  - Syntax highlighting (C, x86-64 asm, Python, shell, JSON, Makefile/Markdown), ELF64 inspector, In-place Hex editor (`e`, `Ctrl+Z`, `Ctrl+S`), Pixel-exact image preview — Kitty graphics (`f=32`) or Sixel (auto-detected via DA1, cell size from `TIOCGWINSZ` / `CSI 16 t`), 2×2 quadrant blocks elsewhere, `DUOP_IMG_MODE=kitty|sixel|blocks|halfblock` to force (PNG incl. palette/16-bit/Adam7, baseline JPEG, GIF, BMP, TGA, PNM, QOI; `Alt+V` and the `F3` "Open as" dialog switch Image / Hex / Text), and Embedded PTY Terminal (`Ctrl+\``).
- **Hybrid Zero-`libc` Architecture**: 100% portable freestanding C23 (`src/core/`, `src/tui/`, `src/fs/`, `src/ui/`, `src/app/`, 171 `.c` files) over a compile-time assembly PAL + SIMD layer (`src/os/`, 28 `.s` files across `linux/x86_64` and `linux/aarch64`), verified by **186 automated test suites**, fault-injection (`tools/faultlab/`), SEU bit-flip tests, and real PTY sessions ([HYBRID_PORTABILITY_PLAN.md](docs/HYBRID_PORTABILITY_PLAN.md)).

---

## Install

### One-line installer (prebuilt static binary + SHA-256 check, falls back to source build)

```bash
curl -fsSL https://raw.githubusercontent.com/BlackKostia/duop/main/scripts/install.sh | bash
```

### Prebuilt Static Binaries (`x86_64` & `aarch64`)

Download `duop-linux-x86_64.tar.gz` or `duop-linux-aarch64.tar.gz` (plus `SHA256SUMS`) from [**GitHub Releases**](https://github.com/BlackKostia/duop/releases/latest).

### Package Managers

| Platform | Command / Recipe |
| --- | --- |
| **Arch Linux (AUR)** | `makepkg -si` in [`packaging/aur/`](packaging/aur/PKGBUILD) |
| **Debian / Ubuntu (`.deb`)** | `./packaging/deb/build-deb.sh && sudo dpkg -i build/deb/duop_*.deb` |
| **Nix (Flakes)** | `nix run github:BlackKostia/duop` or `nix profile install github:BlackKostia/duop` |
| **Homebrew (Linux)** | `brew install --build-from-source ./Formula/duop.rb` |

### Build from Source

Requires Linux (`x86_64` or `aarch64`), `binutils`, `make`, `python3`, and `gcc >= 13` (on `x86_64`) or `clang` (on `aarch64`):

```bash
make release                        # Stripped static Linux x86-64 ELF64 (./duop)
make ARCH=aarch64 release           # Cross-compile static Linux AArch64 ELF64 (clang + aarch64-linux-gnu-binutils)
make ARCH=aarch64 AS=as LD=ld release  # Native build on an AArch64 host
make verify                         # Run modularity, lean-gate, ELF hardening, and 186 PTY test suites
```

Optional shell integration (`cd` into the last directory on exit):

```bash
source /usr/local/share/duop/duop-cd.sh
```

---

## Comparison & Benchmarks

### Duop vs. Popular Terminal File Managers

| Feature / Metric | **Duop (`v2.7.0`)** | `mc` | `yazi` | `lf` | `nnn` |
| --- | --- | --- | --- | --- | --- |
| **Stripped binary size (`x86_64`)** | **187 KiB** (static) | ~1.4 MiB + libs | ~12–20 MiB | ~4.5 MiB | ~120 KiB + libc/ncurses |
| **Runtime dependencies** | **0 (zero-`libc` static ELF)** | `libc`, `glib2`, `ncurses`/`slang` | `libc` (+ optional CLI tools) | `libc` | `libc`, `ncurses`/`readline` |
| **Layout** | **Dual-pane + Tabs + Preview** | Dual-pane | Miller columns | Miller columns | Single-pane |
| **4,798,976-file dir (first frame)** | **26.4 ms** (15.4 MiB RSS) | > 60 s (timeout) | > 60 s (timeout) | > 60 s (timeout) | > 60 s (timeout) |
| **100,000-file dir (first frame)** | **112.7 ms** | ~1.5–3 s | ~0.4–0.9 s | ~0.5–1.2 s | ~0.25–0.5 s |
| **Cold start (`--version`, `hyperfine -N`)** | **2.6 ms** (`/bin/true`: 2.3 ms) | 9.9 ms | ~6–12 ms | ~4–7 ms | ~2.8 ms |
| **Built-in image preview (no helpers)** | **Kitty / Sixel / Unicode blocks** | External | Built-in + plugins | External | Plugins |
| **Built-in Hex editor & ELF64 inspector** | **Yes (with Undo)** | Viewer hex mode | External | External | External |
| **Archive browsing / creation** | Browse & extract (`.tar`/`.gz`/`.zip`); packing planned (`v2.8`) | Full R/W VFS | Via `unar`/`7z` | Via shell scripts | Via shell plugins |
| **Network VFS (`sftp://`)** | Planned out-of-process VFS (`v2.9`) | Built-in | Plugin | External mount | `sshfs` plugin |

### Reproducible Benchmark Numbers

Measured on Linux Mint 22.3 (`Linux 7.0.0-43-generic x86_64`, Intel Core 5 120U, 31 GiB RAM). Reproduce locally with [`scripts/bench.sh`](scripts/bench.sh) and [`scripts/bench_compare.py`](scripts/bench_compare.py) (full 4.8M-file report in [`docs/BENCHMARK_20M_2026-11-17.md`](docs/BENCHMARK_20M_2026-11-17.md)):

```bash
./scripts/bench.sh ./duop
```

| Benchmark | Result (`x86_64`) | Notes |
| --- | ---: | --- |
| Stripped static ELF size | **191,528 B (187 KiB)** | `aarch64`: **214,128 B (209 KiB)**; `.data = 0` |
| Cold process spawn (`hyperfine -N './duop --version'`) | **2.6 ms** | Baseline `/bin/true`: `2.3 ms`; Peak RSS: `264 KiB` |
| 100,000-file directory first frame (PTY) | **112.7 ms** | [`scripts/bench.sh`](scripts/bench.sh) |
| 4,798,976-file directory first frame (PTY) | **26.4 ms median** | Peak RSS `15.43 MiB` ([`docs/BENCHMARK_20M_2026-11-17.md`](docs/BENCHMARK_20M_2026-11-17.md)) |
| Idle CPU wake-ups | **0 / sec** | Infinite `ppoll` timeout + `inotify` kernel events |

---

## Keybindings

| Key | Action | Key | Action |
| --- | --- | --- | --- |
| `Tab` | Switch active pane | `Space` | Mark / unmark entry |
| `F1`–`F10` | Help, Rename, View, Edit, Copy, Move, Mkdir, Del, Preview, Quit | `Alt+P` | Recursive Fuzzy Finder |
| `Ctrl+K` | Command Palette | `Ctrl+F` | Recursive Search / Grep |
| `Ctrl+T` / `Ctrl+W` | New / Close Tab | `Ctrl+D` / `z` | Bookmarks / Frecency |
| `Alt+D` | Disk Usage mode (progress window + `Esc` cancel on long scans) | `Alt+B` | Bulk Rename |
| `Alt+=` | Dir Diff (mark differing/missing) | `Ctrl+\`` | Toggle Embedded PTY Terminal |
| `Alt+K` / `Alt+U` | Package Manager / User Menu | `Ctrl+P` | Toggle Live Preview (`F9`) |
| `Alt+T` / `Alt+L` | Cycle Theme / Next interface language | `Ctrl+Z` | Undo last Trash / Hex byte |

Full reference: [`docs/USER_GUIDE.md`](docs/USER_GUIDE.md) and [`docs/HOTKEYS.md`](docs/HOTKEYS.md).

### Interface Languages (`*.lang`)

English is built in; every other interface language is a `*.lang` pack loaded at startup, so a new language is a new file — no rebuild. Ukrainian ships as [`lang/uk.lang`](lang/uk.lang); copy it as a template:

```text
# ~/.config/duop/lang/de.lang
code = DE
name = Deutsch
hdr_btn_help = " Hilfe "
f_labels.1   = Hilfe
cli_usage    = Aufruf: duop [--lang ID] [LINKS [RECHTS]]\n
```

Keys are the `LangStrings` fields ([`include/types.inc`](include/types.inc)); a missing key falls back to English. `"..."` keeps outer spaces, `\n` `\t` `\"` `\\` are escapes and `{version}` expands to the Duop version. The language is chosen by `--lang <id|code>`, `lang = <id>` in `duoprc`, or `LC_ALL` / `LC_MESSAGES` / `LANG` (`de_DE.UTF-8` → `de`); `Alt+L` cycles all installed languages.

### Keyboard Layouts (`*.layout`)

Hotkeys follow the physical key, so `Alt+T`, `Alt+L`, `j`/`k` or dialog letters work without switching away from a non-Latin layout. The mapping is plain data in `*.layout` files loaded at startup (Ukrainian ships as [`layouts/uk.layout`](layouts/uk.layout)):

```text
# ~/.config/duop/layouts/el.layout
name  = Ελληνικά
keys  = qwertyuiopasdfghjklzxcvbnm
chars = ;ςερτυθιοπασδφγηξκλζχψωβνμ
```

---

## Known Limitations & Roadmap

- **Linux-only for now** (`x86_64` and `aarch64` directly invoke Linux kernel syscalls via the PAL; a FreeBSD PAL backend is planned for `v3.0`).
- **Archives**: browse, view, **unpack** (`F5` in the archive pane, `Alt+F9`) and **test** (`Alt+Shift+F9`) `.tar`, `.tar.gz`, `.zip` / ZIP64 of any size (streamed, CRC-checked, symlink-proof, crash-safe); creation (`Alt+F5`), bzip2 / xz / zstd, and the other items — `TaskQueue` with SHA-256 post-copy verification and `Drop Stack` — are scheduled for `v2.8.x` / `v2.9.x`, see [docs/ARCHIVE_TOOLS_PLAN.md](docs/ARCHIVE_TOOLS_PLAN.md); `TaskQueue` with SHA-256 post-copy verification, and `Drop Stack` are scheduled for `v2.8.x`.
- **Network filesystems (`sftp://`, `docker://`)** are designed as out-of-process VFS packages (`v2.9.x`) so the core binary remains dependency-free.

See [`ROADMAP.md`](ROADMAP.md), [`docs/PRODUCT_STRATEGY_2026-11-20.md`](docs/PRODUCT_STRATEGY_2026-11-20.md), and [`docs/DEVELOPMENT_PLAN_2026-11-17.md`](docs/DEVELOPMENT_PLAN_2026-11-17.md). Contributions are welcome — read [`CONTRIBUTING.md`](CONTRIBUTING.md).

## Developer & License

- **Author**: Kostiantyn Nedbailo (Недбайло Костянтин Геннадійович) — `<ChessPlayerSedoy@gmail.com>`
- **License**: [MIT License](LICENSE)
