# План розвитку Duop (`ROADMAP`)

Цей документ описує покрокову архітектурну дорожню карту розвитку **Duop (`duop`)** — двопанельного TUI-менеджера на гібридній архітектурі без `libc`: **асемблерний PAL + SIMD** (`src/os/`, Linux `x86_64` і `aarch64`) під **100 % портативним freestanding C23** (`src/core|tui|fs|ui|app/*.c`, прямі системні виклики Linux, нуль `libc`).

**Головна стратегічна перевага проєкту**, яку необхідно зберігати й підсилювати на кожному етапі: **миттєвий старт (~1 мс), мізерне споживання RAM, один переносимий статичний ELF без жодних залежностей** (працює будь-де: rescue-диски, мінімальні контейнери, embedded, SSH-сервери без встановлених пакетів).

---

## Поточна база (`v1.0` — `v2.6.91` — реалізовано)

- **Кодова база**: `v1.0` починалася з `13 356` рядків і ~50 КБ (`text=45445, data=0, bss=680`); актуальний `v2.6.91` (171 C23-модуль + 28 асемблерних PAL-модулів, **0 не-PAL `.s` файлів**) із декодерами зображень (JPEG/PNG/GIF/BMP/PNM/TGA/QOI), Kitty/Sixel, Package Manager, `io_uring` та Archive VFS (ZIP64) — stripped static ELF **~183 КБ на `x86_64`** (`text=182683, data=0`) та **~210 КБ на `aarch64`** (`text=210213, data=0`, повний static ELF64 без `libc`).
- **ООП-рантайм** ([duop_oop.h](include/duop_oop.h), [duop_ui.h](include/duop_ui.h), [oop_rt.c](src/core/oop_rt.c), [classes.inc](include/classes.inc)): `Object`, `ScreenBuffer`, `DirModel`, `App`, `Widget` → `HeaderBar`, `FunctionBar`, `StatusBar`, `FilePane`, `PreviewPane`, `Dialog` → `ConfirmDialog`, `InputDialog`, `ViewerDialog`, `HelpDialog`, `ProgressDialog`, `ConflictDialog`, `ChmodDialog`, `InfoDialog`, `ViewAsDialog`, `BookmarkDialog`, `SearchDialog`, `FuzzyDialog`, `UserMenuDialog`, `BulkRenameDialog`, `PkgDialog`.
- **Графіка та ввід**: 24-біт TrueColor diff-рендер ([screen.c](src/tui/screen.c)), 3 теми ([theme.c](src/tui/theme.c)), повний декодер клавіатури (включно з `Alt + UTF-8`) і миші `SGR 1006` ([input.c](src/tui/input.c)).
- **Двомовність (UA / EN)** ([i18n_en.lang](src/tui/i18n_en.lang)): `--lang ua|en`, автовизначення `LC_ALL`/`LC_MESSAGES`/`LANG`, миттєве перемикання `Alt+L` (`Alt+д`) або кнопкою у шапці, динамічні UTF-8 хіт-зони (`utf8_display_width`).
- **Файлова підсистема** ([dir_model.c](src/fs/dir_model.c), [file_ops.c](src/fs/file_ops.c)): `getdents64`, `newfstatat`, Introsort + LSD Radix Sort, живий фільтр, рекурсивні `copy`/`move`/`delete`, Text/Hex viewer і live preview.
- **Автотести** ([test_pty.py](tests/test_pty.py), [test_aarch64_full.py](tests/test_aarch64_full.py)): 182 тестові сюїти у `tests/` (PTY, диференційні тести C vs frozen asm + мутації, повний AArch64 під `qemu-aarch64`, ФС-надійність, `io_uring`, продуктивність, fault-injection, PAL).

---

## Рівень 1 (`v1.1`) — Закрити базові прогалини (щоденний інструмент замість `mc` / `ranger` / `lf`)

### 1.0 `cd-on-exit` (`--choosedir=FILE`)
- **Статус**: ✅ реалізовано (`--choosedir=FILE` / `--choosedir FILE`, [main.s](src/main.s), [app.s](src/app/app.s), [scripts/duop-cd.sh](scripts/duop-cd.sh), [test_choosedir.py](tests/test_choosedir.py)).
- **Файли**: [main.s](src/main.s), [app.s](src/app/app.s), shell-обгортка у `README.md` / `scripts/`.
- **Реалізація**: при виході з прапорцем `--choosedir=FILE` записувати шлях активної панелі у `FILE`, щоб shell (`bash`/`zsh`/`fish`) після закриття `duop` переходив у вибраний каталог.

### 1.1 Конфігурація `~/.config/duop/duoprc` та User Menu
- **Статус**: ✅ реалізовано ([config.s](src/core/config.s), [user_menu_dlg.s](src/ui/user_menu_dlg.s), [theme.s](src/tui/theme.s), [app.s](src/app/app.s), [test_config_usermenu.py](tests/test_config_usermenu.py)).
- **Новий файл**: `src/core/config.s`; інтеграція в [app.s](src/app/app.s) і [theme.s](src/tui/theme.s).
- **Можливості**:
  - перевизначення гарячих клавіш (`bind`);
  - власні RGB-теми (TrueColor);
  - правила відкриття файлів за розширенням (`*.png`, `*.pdf`, `*.mp4` → `$EDITOR` / `xdg-open` / довільна команда);
  - **User Menu / плагіни без інтерпретатора**: кастомні команди в конфігу з плейсхолдерами `%f` (поточний файл), `%F` (усі позначені), `%d`/`%D` (каталог активної/пасивної панелі), `%s` (виділення) — із виводом результату в діалог або панель.
- **Принцип**: зашиті в `.rodata` дефолти гарантують 100% працездатність і без файлу `duoprc`.

### 1.2 Єдиний цикл подій (`poll`/`ppoll`) та автооновлення через `inotify`
- **Статус (`v1.5.0`)**: ✅ один `ppoll` по `stdin` + каналу фонової задачі + `inotify` + каналу прев'ю ([input.s](src/tui/input.s), [app_events.s](src/app/app_events.s)); нескінченний таймаут у простої, точні дедлайни для тостів і дебаунсів; автооновлення обох панелей ([dir_watch.s](src/fs/dir_watch.s)) зі збереженням позначок.
- **Файли**: [app.s](src/app/app.s), [input.s](src/tui/input.s), [pal.inc](include/pal.inc), [pal_target.inc](include/os/linux/x86_64/pal_target.inc).
- **Архітектурна зміна**: винести мультиплексування `SYS_poll`/`SYS_ppoll` із `wait_and_read` ([input.s](src/tui/input.s)) у головний цикл `App_run`, щоб водночас слухати `STDIN_FD`, `inotify_fd` (`SYS_inotify_init1`, `SYS_inotify_add_watch` для лівої й правої панелей), канали фонових задач (`pipe2`) і вбудований PTY без затримок у 200 мс.

### 1.3 Історія навігації, закладки, `frecency` та вкладки панелей
- **Статус**: ✅ реалізовано ([nav_state.s](src/fs/nav_state.s), [bookmark_dlg.s](src/ui/bookmark_dlg.s), [file_pane.s](src/ui/file_pane.s), [app.s](src/app/app.s), [test_nav_tabs.py](tests/test_nav_tabs.py)).
- **Файли**: [classes.inc](include/classes.inc), [file_pane.s](src/ui/file_pane.s), [app.s](src/app/app.s).
- **Функціонал**:
  - історія переходів **Назад / Вперед** (`Alt+←` / `Alt+→`);
  - **Hotlist закладок** (`Ctrl+D`) з діалогом швидкого переходу;
  - вбудований **`frecency`-стрибок** (аналог `zoxide`, команда `z <підрядок>`) із персистентним журналом;
  - **вкладки у кожній панелі** (`Ctrl+T` — нова, `Ctrl+W` — закрити, клік мишею по смужці вкладок, середній клік для закриття).

### 1.4 Асинхронні файлові операції, `copy_file_range` і діалоги прогресу/конфліктів
- **Статус (`v1.2.0`–`v1.6.0`)**: ✅ фоновий `fork`-воркер ([task.c](src/fs/task.c)) зі спільною `MAP_SHARED`-сторінкою прогресу (seqlock для імені файлу) та `pipe2(O_CLOEXEC)`-пробудженням циклу подій через `ppoll` ([input.s](src/tui/input.s)); ✅ `FICLONE` + `copy_file_range` + 256 КіБ буфер у [file_ops.c](src/fs/file_ops.c); ✅ `ProgressDialog` ([progress_dialog.s](src/ui/progress_dialog.s)); ✅ інтерактивний діалог конфліктів `ConflictDialog` ([conflict_dialog.s](src/ui/conflict_dialog.s): `Перезаписати`/`Пропустити`/`Перейменувати`/`Для всіх`).
- **Файли**: [file_ops.c](src/fs/file_ops.c), [dialogs.s](src/ui/dialogs.s), [classes.inc](include/classes.inc) (`TaskQueue`), [pal.inc](include/pal.inc), [pal_target.inc](include/os/linux/x86_64/pal_target.inc).
- **Архітектура**:
  - фонова черга задач у окремому процесі (`fork`/`clone(SIGCHLD)` + двосторонні `pipe2`) замість спільного `CLONE_VM`, що гарантує відсутність data race у алокаторі [memory.s](src/core/memory.s) та глобальних буферах;
  - zero-copy копіювання через `SYS_copy_file_range` (миттєвий reflink на `btrfs`/`xfs`, без копіювання байтів через user-space) з fallback на `read`/`write`;
  - `ProgressDialog`: прогрес-бар, поточний файл, швидкість (`МБ/с`), ETA, пауза/відновлення (`SIGSTOP`/`SIGCONT`) і скасування (`SIGTERM`);
  - інтерактивний діалог конфліктів **`[ Перезаписати ]` / `[ Пропустити ]` / `[ Перейменувати ]` / `[ Для всіх ]`**.

### 1.5 Рекурсивний пошук у віртуальну панель
- **Статус (`v1.6.0`)**: ✅ реалізовано (`Ctrl+F` -> `SearchDialog` ([search_dialog.s](src/ui/search_dialog.s)) з полями маски імені та вмісту `grep`; рекурсивний пошук у [dir_search.s](src/fs/dir_search.s) / [dir_search_match.s](src/fs/dir_search_match.s) у віртуальну панель `DirModel.virtual_mode = 1` із бейджем `[Search]`/`[Пошук]` та переходом `Enter` до оригіналу).
- **Файли**: [dir_model.c](src/fs/dir_model.c), [dialogs.s](src/ui/dialogs.s).
- **Можливості**: пошук за маскою імені та за вмістом файлу (`grep`), завантаження результатів безпосередньо у **віртуальну панель** (`DirModel` у режимі `VIRTUAL_SEARCH`) із переходом до оригіналу по `Enter`.

### 1.6 Кошик (`freedesktop Trash`), `Undo` (`Ctrl+Z`) та розширені атрибути
- **Статус (`v1.6.0`)**: ✅ реалізовано: `Del` -> `$XDG_DATA_HOME/Trash` (`files/` + `info/*.trashinfo`) + 16-рівневий стек `Undo` (`Ctrl+Z`: скасування перейменування та видалення в кошик) у [trash_undo.s](src/fs/trash_undo.s); `ChmodDialog`/`chown` (`Alt+A`, клікабельні біти `rwx` + `uid:gid`) у [attr_dialogs.s](src/ui/attr_dialogs.s); `symlink` (`Alt+S`) / `hardlink` (`Alt+H`); панель детальної інформації (`Alt+I` — `inode`, `nlink`, `uid:gid`, `CRC32`, `SHA-256`) у [info_dialog.s](src/ui/info_dialog.s) та [hash.s](src/core/hash.s).
- **Файли**: [file_ops.c](src/fs/file_ops.c), [dialogs.s](src/ui/dialogs.s), [app.s](src/app/app.s).
- **Кошик + Undo**: переміщення видалених файлів у `$XDG_DATA_HOME/Trash` (`files/` + `info/*.trashinfo`) із можливістю відкату останньої операції (`Ctrl+Z`: перейменування, переміщення, видалення в кошик).
- **Атрибути та посилання**:
  - `ChmodDialog` / `chown` (клікабельні біти `rwx`, вісімкове представлення);
  - створення `symlink` та `hardlink`;
  - панель детальної інформації (`inode`, hardlink count, права, час, `CRC32`/`SHA-256` checksum).

---

## Рівень 2 (`v1.2` – `v2.0`) — «Вау»-можливості, що перевершують `mc` / `ranger` / `lf` / `yazi`

### 2.1 Вбудований Fuzzy Finder + Command Palette (`Ctrl+K`) ✅
- **Файли**: [fuzzy_dialog.s](src/ui/fuzzy_dialog.s), [dir_fuzzy.s](src/fs/dir_fuzzy.s), [app_ext.s](src/app/app_ext.s), [dialogs_ext.inc](include/dialogs_ext.inc) (`FuzzyDialog`).
- **Можливості**:
  - **Command Palette (`Ctrl+K`)**: миттєвий пошук будь-якої дії та комбінації клавіш;
  - **Fuzzy Finder (`Ctrl+P`)** (`fzf`-стиль) по всьому дереву каталогів із підсвічуванням збігів символів і сортуванням за релевантністю.

### 2.2 Перегляд зображень у терміналі ✅
- **Файли**: [img_preview.s](src/ui/img_preview.s), [inflate.s](src/fs/inflate.s), [preview_pane.s](src/ui/preview_pane.s), [app.s](src/app/app.s).
- **Підхід**:
  - **Kitty Graphics Protocol (`f=100`)** передає PNG-файли «як є» без написання PNG-декодера;
  - облік графічного прямокутника, щоб при перекритті діалогами чи зміні файлу коректно надсилати команду очищення (`\x1b_Ga=d...\x1b\\`) і уникати артефактів поверх тексту;
  - fallback на TrueColor half-blocks (`▀`) для SSH/rescue-консолей без Kitty.
- **Формати й режими** ✅: декодери в [src/fs/img_*.c](src/fs/img_core.c) — PNG (усі типи кольору, 1–16 біт, палітра/tRNS, Adam7, файли будь-якого розміру), baseline JPEG (4:2:0/4:2:2/4:4:4, сірий, рестарт-маркери), GIF (LZW, прозорість, interlace), BMP (1/4/8/16/24/32), TGA (+RLE), PNM (P5/P6), QOI; декодування у фоновому помічнику прев'ю ([preview_helper.c](src/fs/preview_helper.c)) з RGBA-мініатюрою; `Alt+V` перемикає прев'ю Зображення / HEX / Текст ([preview_mode.s](src/ui/preview_mode.s)); `F3` на зображенні питає «Відкрити як» ([view_as_dialog.s](src/ui/view_as_dialog.s)), переглядач має режим зображення ([viewer_image.s](src/ui/viewer_image.s)). Тести: [test_image_formats.py](tests/test_image_formats.py). **Якість виводу** ✅: Kitty f=32 (чанки ≤ 4096) і Sixel (адаптивна палітра до 255 кольорів, RLE) з автовизначенням через DA1 та розміром клітинки з TIOCGWINSZ / CSI 16 t ([term_caps.s](src/tui/term_caps.s), [img_sixel.s](src/fs/img_sixel.s), [img_kitty.s](src/fs/img_kitty.s), [img_overlay.s](src/ui/img_overlay.s)); інакше квадрантні блоки 2×2 ([img_quad.s](src/ui/img_quad.s)); кодування — у фоновому помічнику, результат у спільній пам'яті. Тести: [test_image_quality.py](tests/test_image_quality.py). Progressive JPEG і WebP розпізнаються, але показуються як HEX/Текст.

### 2.3 Режим аналізу диска (`Alt+D`, `ncdu`-режим) ✅
- **Файли**: [disk_usage.s](src/fs/disk_usage.s), [du_render.s](src/ui/du_render.s), [file_pane.s](src/ui/file_pane.s).
- **Реалізація**: рекурсивний обхід каталогів на `getdents64`, візуальні TrueColor-бари частки розміру в таблиці, сортування за розміром та навігація всередину каталогів у режимі аналізу.
- **Точність як у `du`** ([pal_du.s](src/os/linux/x86_64/pal_du.s), [du_links.s](src/fs/du_links.s)): зайняті блоки (`st_blocks`) замість видимого розміру, блоки тек, жорсткі посилання один раз; точки монтування псевдо-ФС, FUSE, `squashfs`, `overlay` і мережевих ФС (`fstatfs`) пропускаються. Тести: [test_du_sizes.py](tests/test_du_sizes.py) (DS1–DS2). ✅
- **Прогрес довгого підрахунку** ([du_parallel.s](src/fs/du_parallel.s), [app_du.s](src/app/app_du.s), [du_progress_dlg.s](src/ui/du_progress_dlg.s)): теки рахують робочі потоки, головний потік кожні ~100 мс оновлює вікно «Аналіз розміру тек» (з'являється після `DUOP_DU_PROGRESS_MS`, типово 300 мс): готові теки / усього, смуга, переглянуті теки · файли · байти; `Esc` скасовує обхід без запису часткових розмірів у кеш. Тести: [test_du_progress.py](tests/test_du_progress.py) (DP1–DP3). ✅

### 2.4 Віртуальна файлова система архівів (VFS) ✅
- **Файли**: [dir_model.c](src/fs/dir_model.c) + модуль розпакування (`src/fs/inflate.c`, `src/fs/archive_parse.c`, `src/fs/archive_vfs.c`).
- **Поетапно**:
  1. підтримка `.tar` (просте читання 512-байтних заголовків POSIX/ustar); ✅
  2. компактний `inflate`-модуль (~420 рядків) для прозорої навігації по `.tar.gz` та `.zip` як по звичайних директоріях. ✅

### 2.5 Git-інтеграція в панелях та шапці ✅
- **Файли**: [git_status.c](src/fs/git_status.c), [git_render.s](src/ui/git_render.s), [file_pane.s](src/ui/file_pane.s), [header_bar.s](src/ui/header_bar.s).
- **Реалізація**: виклик `git status --porcelain=v1 -z -b` через `fork/execve` + `pipe`; індикатор гілки в [HeaderBar](src/ui/header_bar.s) та колонка статусів (`M` / `A` / `?` / `D`) у `FilePane`. ✅

### 2.6 Масове перейменування ✅
- **Файли**: [bulk_rename.s](src/ui/bulk_rename.s), [app_ext.s](src/app/app_ext.s).
- **Два режими**:
  - вбудований діалог (`Alt+B`) із живим прев'ю **`Було → Стане`**, заміною підрядків і шаблонами `{n}`, `{name}`, `{ext}`. ✅

### 2.7 Підсвітка синтаксису, ELF64-інспектор та In-place Hex-редактор ✅
- **Файли**: [syntax.s](src/ui/syntax.s), [elf_inspect.s](src/ui/elf_inspect.s), [hex_edit.s](src/ui/hex_edit.s), [preview_pane.s](src/ui/preview_pane.s), [viewer_dialog.s](src/ui/viewer_dialog.s).
- **Підсвітка синтаксису**: табличний лексер для C, x86-64 asm, Python, shell, JSON, Markdown (`Makefile`). ✅
- **ELF64-інспектор**: показ заголовків, секцій і точки входу для виконуваних файлів та `.o`-модулів. ✅
- **Hex-редактор**: редагування байтів прямо в Hex-режимі `ViewerDialog` (`e` для редагування hex-пар, запис у файл `Ctrl+S`, `Ctrl+Z` відкат у межах буфера). ✅

### 2.8 Порівняння/синхронізація панелей, Mouse Drag-and-Drop і контекстне меню ✅
- **Файли**: [dir_diff.s](src/fs/dir_diff.s), [popup_menu.s](src/ui/popup_menu.s), [app_ext.s](src/app/app_ext.s), [app.s](src/app/app.s).
- **Функції**: підсвітка/виділення відмінностей лівої та правої панелей (`Alt+=`) для sync; Mouse Drag-and-Drop (`MACT_DRAG`) між панелями; плаваюче контекстне меню правої кнопки + копіювання шляхів через `OSC 52`. ✅

### 2.9 Вбудований термінал знизу (`Ctrl+\``) ✅
- **Файли**: [pal_pty.s](src/os/linux/x86_64/pal_pty.s), [term_pane.s](src/ui/term_pane.s), [app.s](src/app/app.s), [app_events.s](src/app/app_events.s).
- **Реалізація**: дочірня PTY-сесія (`/dev/ptmx`) із компактним VT-парсером прямо під панелями файлів як неруйнівна альтернатива повному виходу у shell на `Ctrl+O`. ✅
- **Введення** ([term_keys.s](src/ui/term_keys.s)): UTF-8 (кирилиця), `Tab`, `Esc`, стрілки, `Home`/`End`/`PgUp`/`PgDn`/`Del`, `Ctrl+літера`, `Alt+клавіша` передаються у shell; видимий курсор у панелі; F-клавіші лишаються за файловим менеджером. ✅

### 2.10 Розкладки клавіатури як дані (`*.layout`) ✅
- **Файли**: [kbd_layout.s](src/tui/kbd_layout.s) (шлях пошуку: `$DUOP_LAYOUTS`, `~/.config/duop/layouts`, поруч із бінарником через `AT_EXECFN`, `share/duop/layouts`), [kbd_map.s](src/tui/kbd_map.s) (парсер `keys`/`chars`, таблиця `кодова точка → латинська клавіша`), [input.s](src/tui/input.s) (`input_next_event` заповнює `Event.lat`), [layouts/uk.layout](layouts/uk.layout).
- **Реалізація**: жодної вшитої кирилиці в коді — гарячі клавіші порівнюються з `Event.lat`, поля введення отримують `Event.key` без змін; нова розкладка додається файлом без перекомпіляції. Тести: [test_kbd_layout.py](tests/test_kbd_layout.py) (KL1–KL6). ✅

### 2.11 Мовні пакети інтерфейсу (`*.lang`) ✅
- **Файли**: [i18n_en.lang](src/tui/i18n_en.lang) (вбудована англійська — основа і запасний варіант; `tools/gen_i18n.py` компілює її в `build/i18n_en.c`), [i18n_keys.c](src/tui/i18n_keys.c) (ключ ↔ слот `LangStrings`), [i18n_pack.c](src/tui/i18n_pack.c) (парсер `ключ = значення`, `"..."`, `\n`, `{version}`, статична арена без heap), [i18n_env.c](src/tui/i18n_env.c) (реєстр мов, пошук за id / code / локаллю), [data_dirs.c](src/tui/data_dirs.c) (спільний шлях пошуку з розкладками), [lang/uk.lang](lang/uk.lang).
- **Реалізація**: українські рядки, палітра команд і `--help`/`--version` винесені з коду у пакет; нова мова — файл без перекомпіляції; `Alt+L` перебирає всі мови; `--lang`, `duoprc lang =`, `LANG`. Тести: [test_lang_packs.py](tests/test_lang_packs.py) (LP1–LP7). ✅

---

## Рівень 3 — Популяризація, дистрибуція та переносимість

1. **Англомовний `README.md` + переклад та візуальні матеріали ✅**: головний [README.md](README.md) англійською мовою, окремий [README.uk.md](README.uk.md), детермінований генератор SVG-скриншотів [tools/screenshot_svg.py](tools/screenshot_svg.py) (`docs/media/*.svg`) та порівняльна таблиця з конкурентами. ✅
2. **Публічні бенчмарки у `README` ✅**:
   - розмір stripped ELF-бінарника (`191 528 Б` / `187 КіБ` на `x86_64`, `214 128 Б` / `209 КіБ` на `aarch64`, `.data=0`), час холодного старту (`2.6 ms` через `hyperfine -N`), швидкість відкриття `100 000` файлів (`112.7 ms`, [scripts/bench.sh](scripts/bench.sh)) та `4 798 976` файлів (`26.4 ms`, `15.4 MiB RSS`, [docs/BENCHMARK_20M_2026-11-17.md](docs/BENCHMARK_20M_2026-11-17.md)). ✅
3. **CI та дистрибуція ✅**:
   - GitHub Actions CI ([.github/workflows/ci.yml](.github/workflows/ci.yml)) із автоматичним `release`-job та генератором релізних артефактів [scripts/make_dist.sh](scripts/make_dist.sh) (`x86_64` + `aarch64` + `SHA256SUMS`); ✅
   - пакети для `AUR` ([packaging/aur/PKGBUILD](packaging/aur/PKGBUILD)), `.deb` ([packaging/deb/](packaging/deb/build-deb.sh)), `Nix` ([flake.nix](flake.nix)), `Homebrew` ([Formula/duop.rb](Formula/duop.rb)) та однорядковий [scripts/install.sh](scripts/install.sh) із перевіркою `SHA256SUMS`. ✅
4. **Порт на AArch64 (compile-time PAL ✅, повний runtime-бінарник ✅)**:
   - повний compile-time PAL-бекенд у [include/os/linux/aarch64/pal_target.inc](include/os/linux/aarch64/pal_target.inc) та [src/os/linux/aarch64/](src/os/linux/aarch64/pal_os.s) разом зі 100% портативними C23-шарами (`v2.6.90+`) збирають повний статичний ELF64-бінарник `duop` для `linux/aarch64`, верифікований під `qemu-aarch64` та на нативному ARM64 runner у CI. ✅
5. **Підготовка релізу `v2.7.0` та Go-to-Market матеріали ✅ (публікація тегу й анонсів ⏳)**:
   - продуктова стратегія та SWOT-аналіз у [docs/PRODUCT_STRATEGY_2026-11-20.md](docs/PRODUCT_STRATEGY_2026-11-20.md), інструкція для контриб'юторів [CONTRIBUTING.md](CONTRIBUTING.md), шаблони Issues/PR у `.github/` та готові тексти анонсів (Hacker News, Reddit, Lobsters, DOU) у `docs/launch/`.

---

## Рівень 4 (`v2.7+`) — Невиконані завдання та ідеї розвитку (Product Roadmap)

### 4.1 Повноцінна підтримка архівів (`v2.7.1` → `v2.15.x`, плани: [ARCHIVE_TOOLS_PLAN.md](docs/ARCHIVE_TOOLS_PLAN.md) — фази 0–2, [ARCHIVE_ROADMAP_2026-11-24.md](docs/ARCHIVE_ROADMAP_2026-11-24.md) — далі)
- [x] **Фаза 0 (`v2.7.1`)**: `F5`/`F6`/`F8` в архіві показують «Archive is read-only» замість зламаного діалогу; регресійний тест `tests/test_arc_readonly.py`.
- [x] **Фаза 1 (`v2.8.0`) — масштаб і потоковість**: `mmap`-вхід (`arc_source.c`), динамічний відсортований індекс (`arc_index.c`), потоковий `DEFLATE`/Deflate64 (`inflate_stream.c`) і gzip з CRC (`gzip_dec.c`), повний `tar` (ustar prefix, GNU `L/K`, PAX, base-256, `arc_tar.c`) і `zip` (UTF-8/CP437, ZIP64, префікс SFX, перекриття, `arc_zip.c`), потокове розпакування з перевіркою CRC (`arc_extract.c`); ліміти 4 МіБ / 1024 / 8 МіБ / 64 МіБ зняті.
- [x] **Фаза 2 (`v2.9.0`) — розпакування і тест**: `F5` в архіві, `Alt+F9` (розпакувати) і `Alt+Shift+F9` (перевірити) на `arc_unpack*.c`/`arc_path.c`/`arc_task.c` (`TASK_OP_UNPACK`/`TASK_OP_ARCTEST` у `TaskWorker`): `openat(O_NOFOLLOW)`-обхід, Zip-Slip/symlink/hardlink-захист, `.duop-part` + CRC + `rename`, конфлікти, `smart unpack`, ліміт місця і захист від бомби, прогрес і скасування.
- [ ] **Фаза 3 (`v2.9.x`) — фундамент**: [x] визначення формату за магією (`arc_detect.c`) і одиночний `.gz` (`v2.9.3`); [ ] асинхронне відкриття стиснутого tar (worker + прогрес), [ ] прев'ю й інфо-рядок архіву, [ ] конфіг-ключі лімітів.
- [ ] **Фаза 4 (`v2.10.x`) — декодери**: [x] `zstd` (`zstd_dec/fse/huf/seq.c`, XXH64; `.zst`, `.tar.zst`, zip-метод 93; диференційно проти `zstd`, `v2.10.0`); [x] `xz`/LZMA2 (`lzma_dec/lzma2_dec/xz_dec.c`, CRC-64; `.xz`, `.tar.xz`, zip-метод 95; диференційно проти liblzma, `v2.10.1`; BCJ/Delta і LZMA1/zip-метод 14 — далі); [x] `bzip2` (`bzip2_dec.c`; `.tar.bz2`, `.bz2`, zip-метод 12; диференційно проти libbz2, `v2.10.2`); [ ] LZMA1 (`.lzma`, zip-метод 14) і фільтри BCJ/Delta для xz.
- [ ] **Фаза 5 (`v2.11.x`) — контейнери**: `.deb`/`.ar`, `.rpm`/`.cpio`, OCI-шари, `.7z` (читання), `.iso`, вкладені архіви.
- [ ] **Фаза 6 (`v2.12.x`) — пакування** ([x] DEFLATE-компресор `deflate.c`/`deflate_huff.c`, `v2.10.4`): `Alt+F5` → `.tar`, `.tar.gz`, `.zip`/ZIP64 (`deflate.c`, `tar_write.c`, `zip_write.c`, `archive_pack.c`), детермінований вивід, перевірка після пакування; далі запис `zstd`.
- [ ] **Фаза 7 (`v2.13.x`)**: додавання/видалення/перейменування записів в архіві (через temp + `rename`).
- [ ] **Фаза 8 (`v2.14.x`)**: шифрування (ZipCrypto — лише читання, WinZip AES-256, 7z AES).
- [ ] **Фаза 9 (`v2.15.x`)**: кеш індексу й довільний доступ, SIMD CRC, паралелізм, headless CLI `duop x|a|t|l`, міграція `pkg_archive.c`, бенчмарк. Повний план — [ARCHIVE_ROADMAP_2026-11-24.md](docs/ARCHIVE_ROADMAP_2026-11-24.md).

### 4.2 Щоденні DevOps / SRE / Dev сценарії (ідеї розвитку — не реалізовано)
- [ ] **Віддалена панель `sftp://` через `pkg_vfs`** (`virtual_mode = 3` у [pkg_vfs_pane.c](src/fs/pkg_vfs_pane.c)): навігація та копіювання файлів по SSH без вшивання TLS/SSH-бібліотек у статичний бінарник.
- [ ] **Live Log-Tailing у `ViewerDialog`** (`F` / follow-режим): відстеження дописуваних логів у реальному часі поверх `mmap` + `inotify`.
- [ ] **Side-by-side Text Diff у переглядачі**: построкове порівняння вмісту двох файлів із лівої та правої панелей (доповнення до `Alt+=` для директорій).
- [ ] **Мікро-образ для debug-контейнерів**: офіційний `FROM scratch` OCI-образ / сценарій швидкої доставки в `kubectl debug` та `docker cp` як автономного rescue-інструменту.

### 4.3 Безпека екосистеми, кросплатформність і техборг (не реалізовано)
- [ ] **Криптографічний підпис реєстру пакетів (`registry.tsv`)**: перевірка Ed25519/minisign-підпису індексу розширень (на додачу до наявного SHA-256 архівів) та курировані VFS-пакети (`docker://`, `systemd://`, `s3://`).
- [ ] **Порт під Windows (`duop.exe` через MSYS2 / Cygwin POSIX PAL) та FreeBSD**: окремий ізольований compile-time PAL у `src/os/windows/x86_64/` (~520 рядків C, 0 змін у 171 прикладному модулі, детальний план у [WINDOWS_PORT_PLAN.md](docs/WINDOWS_PORT_PLAN.md)) та `src/os/freebsd/` (після підтвердження попиту від спільноти).
- [ ] **Закриття техборгу в [`lean_baseline.json`](.agents/skills/duop-lean-modules/scripts/lean_baseline.json)**: декомпозиція 5 великих модулів (зокрема [file_pane_render.s](src/ui/file_pane_render.s) — 1223 рядки) і 39 функцій понад бюджет у 80 рядків (`StatusBar_render`, `ScreenBuffer_flush`, `DirModel_step_load` тощо).

---

## Аудит продуктивності (`v1.3.0` – `v1.5.0`) — виконано

- **`v1.3.0`**: slab-алокатор ([memory.s](src/core/memory.s)), SSE2-примітиви в PAL ([pal_simd.s](src/os/linux/x86_64/pal_simd.s)), FD-відносні copy/delete з `FICLONE` → `copy_file_range` → спільний буфер 256 КіБ, видалення за `d_type` без `stat`, prefix-key radix для `SORT_NAME`/`SORT_EXT`, звуження фільтра без пересортування.
- **`v1.4.0`**: LRU-кеш тек ([dir_cache.c](src/fs/dir_cache.c)) і лексичний `chdir` без `chdir`/`getcwd`, `mmap`-переглядач будь-якого розміру із захистом від `SIGBUS` ([viewer_dialog.s](src/ui/viewer_dialog.s)), ковзне вікно `io_uring` на 256 запитів і пакетний `statx` для цілей симлінків, почергове завантаження обох панелей.
- **`v1.5.0`**: мультиплексований цикл подій без холостих пробуджень, `inotify`-автооновлення, дебаунс прев'ю й асинхронне читання в окремому процесі ([preview_io.c](src/fs/preview_io.c)), DEC 2026 synchronized output, SIMD-пропуск незмінених рядків у `ScreenBuffer_flush`, табличні RGB-послідовності, кеш `format_datetime`.

## Аудит відмовостійкості (`v2.6.x`, 2026-11-16) — виконано

Критичний аудит «космічного» рівня надійності: усі знайдені дефекти закрито з регресійними тестами (TDD, спершу RED).

- **Падіння та пошкодження пам'яті**: прапор `IORING_SETUP_CLOEXEC` збігався з `NO_SQARRAY` (`io_uring` не стартував) → прибрано, додано перевірку масиву SQ і меж CQ ([pal_uring.s](src/os/linux/x86_64/pal_uring.s)); переповнення купи в ZIP-розпаковці та живі caller-saved регістри через `call` (`ds_append_entry`, `arc_extract_entry`, `FilePane_render__build`, курсор пошуку, смуга прокрутки, `handle_mouse__part2`); `DirModel_sort` на 1 елементі; крихітні розміри терміналу (`term_pane`); вихід палітри BMP за межі файлу.
- **Нові аудити-гейти**: [audit_live_regs.py](.agents/skills/duop-reliability-audit/scripts/audit_live_regs.py) (бінарний аналіз «caller-saved регістр живий через `call`», baseline із поясненнями, у `verify_build.sh`), вимога `PT_GNU_STACK` без виконуваного стека, fuzz-харнес зі сторожовими сторінками для декодерів зображень.
- **Безпека виконання команд**: макроси `%f %p %d %D %F %s` екрануються за контекстом лапок і ніколи не виконуються усіченими ([config_expand.s](src/core/config_expand.s), [app_cmdline.s](src/app/app_cmdline.s)); `git status` лише для довірених репозиторіїв (без `[filter]`/`[include]`), `core.fsmonitor=false`, таймаут 2,5 с ([git_trust.s](src/fs/git_trust.s)).
- **Цілісність даних**: перезапис через тимчасовий файл і `rename` ([file_ops_overwrite.s](src/fs/file_ops_overwrite.s)), `RENAME_NOREPLACE` для відновлення з кошика, кошик за freedesktop Trash 1.0 ([trash_info.s](src/fs/trash_info.s)), неблокуюче відкриття (FIFO у UI-потоці більше не вішає програму), `duoprc` до 1 МіБ із попередженням замість мовчазного ігнорування.
- **Залишкові обмеження**: перенесення в кошик між файловими системами досі копіює в UI-потоці; атомарним є лише перезапис (нові файли пишуться напряму); `DeletionDate` — UTC.

## Рекомендований порядок реалізації

1. **`--choosedir=FILE` (`cd-on-exit`)** + shell-обгортка → миттєва щоденна придатність.
2. ✅ **Уніфікований `poll`/`ppoll`-цикл + `inotify`** (`v1.5.0`).
3. **Конфіг `duoprc`** (`src/core/config.s` + User Menu `%f %F %d %D %s` + розширення відкриття).
4. **Історія (`Alt+←/→`), закладки (`Ctrl+D`), `frecency` (`z`), вкладки (`Ctrl+T`/`Ctrl+W`)**.
5. ✅ **Асинхронні операції (`fork`+`pipe2`+`copy_file_range`+`ProgressDialog` ✅ `v1.2.0`–`v1.6.0`) + діалог конфліктів + Trash/Undo**.
6. **Fuzzy finder + Command Palette (`Ctrl+K`) + рекурсивний пошук у віртуальну панель**.
7. **`ncdu`-режим (паралельний `getdents64`)** + бенчмарки на 100k файлів.
8. **Kitty-прев'ю зображень (`f=100`) + Git-колонка + `tar`/`zip` VFS**.
9. **Підсвітка синтаксису + Hex-редактор + масове перейменування (`vidir`/шаблони) + порівняння панелей + вбудований PTY-термінал**.
10. *Паралельно*: англійський `README`, GIF-демо, GitHub Actions CI, GitHub Releases/пакети та порт на AArch64.

---

## Матриця ключових файлів

| Файл | Основне призначення у плані розвитку |
|---|---|
| [src/main.s](src/main.s) | Обробка `--choosedir=FILE`, старт конфігу та CLI-прапорців |
| `src/core/config.s` *(новий)* | Парсер `~/.config/duop/duoprc`, біндинги клавіш, кастомні теми, асоціації та User Menu |
| [src/app/app.s](src/app/app.s) | Єдиний цикл `poll`/`ppoll` (`STDIN_FD`, `inotify`, фонові воркери, `git`), `CommandPalette`, `Undo` |
| [src/fs/file_ops.c](src/fs/file_ops.c) | `copy_file_range`, асинхронна черга задач `TaskQueue`, `XDG Trash`, `Undo`-журнал, `chmod`/`chown`/`link` |
| [src/fs/dir_model.c](src/fs/dir_model.c) | Віртуальні панелі (пошук, `.tar`/`.zip` VFS, `ncdu`-дерево), кешування Git-статусу |
| [src/ui/dialogs.s](src/ui/dialogs.s) | `ProgressDialog`, діалоги конфліктів, `ChmodDialog`, `CommandPalette`, `FuzzyDialog`, Hex-edit, `BulkRename` |
| [src/ui/file_pane.s](src/ui/file_pane.s) | Смужка вкладок (`Tabs`), колонка Git-статусів, смужки розміру `ncdu`, підсвітка `DirDiff` |
| [src/ui/preview_pane.s](src/ui/preview_pane.s) | `Kitty f=100` / `Sixel` / half-block прев'ю, таблична підсвітка синтаксису, `ELF64`-інспектор |
| [include/classes.inc](include/classes.inc), [include/types.inc](include/types.inc), [include/config.inc](include/config.inc), [include/pal.inc](include/pal.inc) | Структури `TaskQueue`, `Config`, `History`, `FuzzyDialog`, константи нових клавіш і системних викликів (`inotify`, `copy_file_range`, `fchmodat`, `ppoll`, `pipe2`) |

---

## Критерії готовності та перевірки (Definition of Done)

1. **Чистий static ELF без `libc`**: збірка `make release` і контроль розміру секцій (`size duop`).
2. **ООП та двомовність**: кожен новий віджет/діалог наслідує `Widget`/`Dialog`, усі тексти додаються в `src/tui/i18n_en.lang` та `lang/uk.lang`, хіт-зони миші розраховуються через `utf8_display_width`.
3. **PTY-тести**: кожна нова фіча покривається автоматичним сценарієм у [test_pty.py](tests/test_pty.py) (`make clean && make && make test`).
4. **Бенчмарки**: автоматизований замір часу старту (`hyperfine`), пам'яті (`/usr/bin/time -v`) і пропускної здатності каталогу на `100k` файлів.
