# Duop v2.11.0 — Complete Archive Support (`x86_64` & `aarch64`)

**Duop** (`duop`) is a dual-pane terminal file manager for Linux shipped as a single static ELF64 binary with **zero external runtime dependencies** — no `libc`, no `ncurses`, no shared libraries (`255,032 B` / `249 KiB` on `x86_64`, `292,312 B` / `285 KiB` on `aarch64`, `.data = 0`).

## Highlights of `v2.11.0` (since `v2.7.0`)

- **Archives, end to end** — browse, view, unpack, test and create, all in freestanding C23 with no third-party code:
  - **Reading** (any size, streamed, checksum-verified, kind detected by contents rather than name): `.tar`, `.zip` / ZIP64 (incl. `.docx`, `.jar`, `.apk`, `.whl`, …), `.gz`, `.xz` (LZMA2; CRC-32 / CRC-64 / SHA-256 checks), `.bz2`, `.zst` and the `.tar.*` forms; zip members compressed with Deflate64, bzip2, zstd and xz.
  - **Unpacking** (`F5` in an archive pane, `Alt+F9`) and **testing** (`Alt+Shift+F9`) in the background worker: Zip-Slip / symlink / hardlink protection, `openat(O_NOFOLLOW)` walks, temp file + CRC check + atomic rename, smart unpack (no "tarbombs"), decompression-bomb guard, conflict dialog, progress and cancel.
  - **Creating** (`Alt+F5`): `.tar`, `.tar.gz` / `.tgz`, `.zip` (own DEFLATE compressor, output size equal to zlib's at levels 6 and 9; ZIP64, PAX headers, UTF-8 names, timestamps, Unix modes). Sorted walk that never follows symlinks, written to a temporary sibling, fsynced and renamed without replacing anything.
- **Verified against the real tools**: differential tests against `zstd`, liblzma, libbz2, zlib, `tar`, `unzip` and `7z`; fuzzing, hand-crafted streams with fixed-up checksums, mutation checks per module, `qemu-aarch64` equality.
- **Extreme Directory Throughput** (measured on `v2.7.0`, see the [benchmark report](https://github.com/BlackKostia/duop/blob/main/docs/BENCHMARK_20M_2026-11-17.md)): **26.4 ms** median first frame on a **4,798,976-file directory**; **1.3 ms** cold spawn (`hyperfine -N`), **0 idle CPU wake-ups**.
- **Built-in Developer & SRE Tooling**: Command Palette (`Ctrl+K`), Recursive Fuzzy Finder (`Alt+P`), Recursive Grep Virtual Panel (`Ctrl+F`), Tabs, Bookmarks, Frecency, pixel-exact image preview (Kitty / Sixel / blocks), syntax highlighting, ELF64 inspector, Hex editor with Undo, Git badges, Disk Usage (`Alt+D`), Bulk Rename (`Alt+B`), Dir Diff (`Alt+=`), XDG Trash + Undo, embedded PTY terminal (`Ctrl+\``).
- **Data-driven localization** (`lang/*.lang`, English + Ukrainian included) and physical keyboard layout maps (`layouts/*.layout`).
- **Verified reliability**: **204 automated test suites** (real PTY sessions, differential C23-vs-ASM oracle checks, `qemu-aarch64`, fault-injection via `faultlab`, SEU bit-flip tests); the release was built and tested locally before tagging.

## Known limits

`.7z`, `.rar`, `.deb` / `.rpm` / `.iso`, BCJ/Delta-filtered xz, `.lzma`, writing `.zst`, in-place archive editing and encryption are not supported yet; very large `.tar.xz` / `.tar.bz2` are unpacked by `Alt+F9` rather than browsed (the browse step is synchronous). See [`docs/ARCHIVE_ROADMAP_2026-11-24.md`](https://github.com/BlackKostia/duop/blob/main/docs/ARCHIVE_ROADMAP_2026-11-24.md).

## Quick Install

```bash
curl -fsSL https://raw.githubusercontent.com/BlackKostia/duop/main/scripts/install.sh | bash
```

Or download `duop-linux-x86_64.tar.gz` / `duop-linux-aarch64.tar.gz` below and verify against `SHA256SUMS`:

```bash
sha256sum -c SHA256SUMS --ignore-missing
```

Full documentation: [`README.md`](https://github.com/BlackKostia/duop/blob/main/README.md), [`docs/USER_GUIDE.md`](https://github.com/BlackKostia/duop/blob/main/docs/USER_GUIDE.md), [`docs/HOTKEYS.md`](https://github.com/BlackKostia/duop/blob/main/docs/HOTKEYS.md).
