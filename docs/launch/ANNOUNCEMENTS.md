# Готові тексти анонсів для релізу Duop `v2.11.0`

Нижче зібрано готові до публікації тексти для основних майданчиків плюс чекліст запуску.

---

## Чекліст перед публікацією (5 хвилин)

1. **Перейменувати репозиторій на GitHub**:
   - Відкрити `https://github.com/BlackKostia/NC/settings` → **Repository name** → змінити `NC` на `duop` → **Rename**. (GitHub автоматично перенаправляє всі старі `git clone` та посилання).
   - Оновити локальний remote:
     ```bash
     git remote set-url origin https://github.com/BlackKostia/duop.git
     ```
2. **Опублікувати код і тег `v2.11.0`** (CI автоматично збере `x86_64` і `aarch64` та створить GitHub Release):
   ```bash
   git push origin main
   git tag -a v2.11.0 -m "Duop v2.11.0"
   git push origin v2.11.0
   ```
3. **Оновити SHA-256 тарбола після створення тегу** (для AUR та Homebrew):
   ```bash
   curl -sL https://github.com/BlackKostia/duop/archive/refs/tags/v2.11.0.tar.gz | sha256sum
   ```

---

## 1. Hacker News (`Show HN`)

**Title:**
`Show HN: Duop – A 249 KiB zero-libc dual-pane terminal file manager for Linux (x86_64/ARM64)`

**URL:**
`https://github.com/BlackKostia/duop`

**First Comment (авторський коментар відразу після публікації):**
> Hi HN! I built **Duop** (`duop`), a dual-pane terminal file manager for Linux (`x86_64` and `aarch64`) that ships as a single ~249 KiB static ELF binary with zero external dependencies — no `libc`, no `ncurses`, no dynamic linker.
>
> **Why build another TUI file manager?**
> two reasons:
> 1. **Production servers, minimal containers, and rescue environments**: I wanted a single tiny binary I could `scp` or `docker cp` onto any Linux box (x86_64 or ARM64) without worrying about glibc/musl mismatches, terminfo databases, or external helper tools for image/hex/archive inspection.
> 2. **Extreme directory sizes**: on a test directory with 4.8 million files (`4,798,976` entries), `mc`, `yazi`, `lf`, `nnn`, `ranger`, and `vifm` all timed out past 60 seconds sorting before drawing. Duop renders its first interactive frame in **26.4 ms** using **15.4 MiB RSS** by streaming `getdents64` into a compact 32-byte `FileEntry` + contiguous `name_pool` arena, lazy `d_type` rendering, and rolling `io_uring` (`IORING_OP_STATX`) batch statting.
>
> **How it's built:**
> - **Architecture**: Duop started in pure x86-64 GNU Assembler and was migrated module-by-module (using differential tests against frozen assembly oracles) to **100% portable freestanding C23** (`-ffreestanding -nostdlib`, 201 `.c` files) over a thin compile-time **assembly PAL + SIMD layer** (28 `.s` files across `linux/x86_64` and `linux/aarch64`).
> - **Event loop**: single `ppoll` loop with an infinite idle timeout (0 CPU wake-ups when idle) + `inotify` live directory refresh + `DEC 2026` synchronized terminal output.
> - **Built-in tooling (no external binaries required)**: TrueColor themes, Command Palette (`Ctrl+K`), recursive fuzzy finder (`Alt+P`) and grep virtual panel (`Ctrl+F`), pixel-exact image preview (Kitty graphics `f=32`, Sixel, or Unicode quadrant blocks for PNG/JPEG/GIF/BMP/TGA/PNM/QOI decoders written from scratch), in-place Hex editor with Undo, ELF64 inspector, read/extract archive VFS (`.tar`, `.tar.gz`, `.zip`), background `fork` copy/move with `FICLONE`/`copy_file_range`, and an embedded PTY terminal (`Ctrl+\``).
> - **Testing**: 204 automated test suites (real PTY sessions, fault-injection campaigns on syscalls/allocations, SEU bit-flip tests, and `qemu-aarch64` CI).
>
> **Current limitations:**
> - Linux-only (`x86_64` & `aarch64`; a FreeBSD PAL is planned).
> - Archives: browse/unpack/test/create (`.tar`, `.tar.gz`, `.zip`; read `.gz`/`.xz`/`.bz2`/`.zst` too); `.7z`/`.rar`, in-place archive editing and encryption are not there yet, and a sequential `TaskQueue` with SHA-256 verification is next on the roadmap.
>
> Happy to answer any questions about zero-`libc` C23, `io_uring`, terminal graphics, or the assembly-to-C23 differential migration!

---

## 2. Reddit (`r/linux`, `r/commandline`)

**Title:**
`Duop v2.11.0 – A 249 KiB zero-libc dual-pane TUI file manager for Linux (x86_64 & aarch64) with built-in Kitty/Sixel image preview, fuzzy finder, hex/ELF inspector, and 26 ms open time on 4.8M files`

**Body:**
> **Repository & Screenshots:** https://github.com/BlackKostia/duop
>
> **Duop** (`duop`) is an MIT-licensed dual-pane terminal file manager for Linux built with **zero external runtime dependencies** (no `libc`, no `ncurses` — a single static ELF64 binary of **249 KiB** on `x86_64` and **285 KiB** on `aarch64`).
>
> ### Highlights
> - **Drop-in static binary**: works anywhere on Linux (`scp` / `docker cp` into scratch/alpine/distroless containers or embedded ARM64 boards).
> - **Fast on huge directories**: opens a 100,000-file directory in **112.7 ms** and renders the first frame on a **4,798,976-file directory in 26.4 ms** (15.4 MiB RSS) via streaming `getdents64`, 32-byte `FileEntry` arenas, and rolling `io_uring` `STATX`.
> - **0 wake-ups when idle**: `ppoll` event loop + `inotify` automatic directory refresh.
> - **Batteries included (no external CLI dependencies needed)**:
>   - Built-in decoders for **PNG, JPEG, GIF, BMP, TGA, PNM, QOI** rendered via **Kitty graphics (`f=32`)**, **Sixel**, or 2×2 Unicode blocks.
>   - **Command Palette** (`Ctrl+K`), **Recursive Fuzzy Finder** (`Alt+P`), **Recursive Grep Virtual Panel** (`Ctrl+F`), Tabs (`Ctrl+T`), Bookmarks (`Ctrl+D`), Frecency (`z`).
>   - **In-place Hex Editor** (with Undo), **ELF64 Inspector**, syntax highlighting, **Archive VFS** (`.tar`, `.tar.gz`, `.zip`), **Disk Usage analyzer** (`Alt+D`), **Bulk Rename** (`Alt+B`), **Dir Diff** (`Alt+=`), XDG Trash + `Ctrl+Z` Undo, and an **Embedded PTY Terminal** (`Ctrl+\``).
>   - **Data-driven i18n & keyboard layouts**: add a new interface language by dropping a `*.lang` file (English and Ukrainian included) or a non-Latin physical hotkey map via `*.layout` without recompiling.
>
> ### Try it
> ```bash
> curl -fsSL https://raw.githubusercontent.com/BlackKostia/duop/main/scripts/install.sh | bash
> ```
> Or build from source (`make release`) / use the AUR (`PKGBUILD`), `.deb`, Nix flake, or Homebrew formula in the repo.
>
> Feedback, benchmarks on your hardware, and language pack PRs (`lang/*.lang`) are very welcome!

---

## 3. Українська спільнота (DOU / Telegram / Habr)

**Заголовок:**
`Duop v2.11.0 — двопанельний файловий менеджер для термінала Linux розміром 249 КіБ без libc (x86_64 та ARM64)`

**Текст:**
> Вітаю спільноту! Хочу поділитися власним open-source проєктом **Duop** (`duop`) — ультрашвидким двопанельним файловим менеджером для термінала Linux (`x86_64` та `aarch64`), який постачається як єдиний статичний ELF64-бінарник розміром **249 КіБ** без жодних зовнішніх залежностей (без `libc`, без `ncurses`, лише прямі системні виклики ядра).
>
> 🔗 **GitHub (MIT License, українська локалізація з коробки):** https://github.com/BlackKostia/duop
>
> ### Чим Duop відрізняється від `mc`, `yazi` та `lf`?
> 1. **Один статичний файл (`249 КіБ` на x86_64 / `285 КіБ` на ARM64)**: можна закинути через `scp` або `docker cp` на будь-який сервер, у мінімальний контейнер чи на одноплатник — нічого встановлювати не потрібно.
> 2. **Швидкість на мільйонах файлів**: на тестовому каталозі з **4 798 976 файлів** перший інтерактивний кадр з'являється за **26,4 мс** при споживанні **15,4 МіБ ОЗП** (у цьому ж тесті `mc`, `yazi`, `lf`, `nnn`, `ranger` та `vifm` не вкладаються у 60-секундний таймаут).
> 3. **Все всередині без сторонніх утиліт**:
>    - власні декодери зображень (**PNG, JPEG, GIF, BMP, TGA, PNM, QOI**) з виводом піксель-у-піксель через **Kitty graphics**, **Sixel** або Unicode-блоки;
>    - **Command Palette** (`Ctrl+K`), рекурсивний **Fuzzy Finder** (`Alt+P`), рекурсивний **Grep** у віртуальну панель (`Ctrl+F`), вкладки, закладки, frecency;
>    - вбудований **Hex-редактор** із Undo, інспектор **ELF64**, підсвітка синтаксису, перегляд архівів (`.tar`, `.tar.gz`, `.zip`), аналізатор диска (`Alt+D`), масове перейменування (`Alt+B`), порівняння тек (`Alt+=`), XDG Trash із `Ctrl+Z` та вбудований **PTY-термінал** (`Ctrl+\``);
>    - повна **українська та англійська локалізація** (`Alt+L`), а гарячі клавіші працюють навіть при увімкненій українській розкладці завдяки файлам `layouts/*.layout`.
> 4. **Архітектура та надійність**: 201 модуль на freestanding C23 поверх асемблерного PAL/SIMD шару (`x86_64` + `aarch64`), 204 автоматизовані тестові набори (реальні PTY-сесії, fault-injection системних викликів, SEU bit-flip тести).
>
> Буду радий фідбеку, зірочкам на GitHub та ідеям для наступних релізів (`v2.8` — потокове пакування архівів і черга задач із SHA-256 верифікацією, `v2.9` — out-of-process `sftp://` та `docker://` VFS)!
