# План повноцінної підтримки архівів у Duop (v2.7.1 → v2.9.x)

> Замінює попередній план на асемблері (`inflate_stream.s`, `Alt+P`): тепер усе — freestanding C23, `Alt+P` зайнята Fuzzy Finder. Статус: **Фази 0–2 виконані (v2.7.1 → v2.9.0)**; далі — Фаза 3 (пакування); прогрес — у [ROADMAP.md](../ROADMAP.md) §4.1.

## Context

Зараз Archive VFS — **лише перегляд**: `.tar`, `.tar.gz/.tgz`, `.zip` (ZIP64). Запит: повна підтримка — читання, розпакування, перевірка, створення. Узгоджено з користувачем:
- **Формати читання/розпаку:** tar, zip/ZIP64, tar.gz + кодеки **bz2, xz (LZMA2), zstd** (і окремі `.gz/.bz2/.xz/.zst`, `.tar.bz2/.xz/.zst`).
- **Створення:** `tar`, `tar.gz`, `zip` (власний DEFLATE-компресор).
- **Гарячі клавіші:** `Alt+F5` — пакувати, `Alt+F9` — розпакувати; `F5` у панелі-архіві розпаковує в протилежну панель. (`Alt+P` зайнята Fuzzy Finder — старі плани застаріли.)

Пріоритет проєкту — **відмовостійкість** (вимога space-industry fault tolerance); архівні дані недовірені → fuzz, faultlab, crash-consistency обов'язкові на кожній фазі.

## Що є зараз (виявлені прогалини)

| Область | Стан у коді |
|---|---|
| Ліміти | `ARC_MAX_FILE`=4 МіБ, `ARC_MAX_ENTRIES`=1024, tgz ≤ 8 МіБ, zip-запис ≤ 64 МіБ цілком у RAM ([duop_arc.h](include/duop_arc.h)); решта архіву мовчки обрізається (`arc_truncated`) |
| Розпаковування | Лише `F3` одного запису в temp ([archive_vfs.c](src/fs/archive_vfs.c) `arc_extract_entry`). **`F5` з панелі-архіву зламаний:** `arc_populate` ставить `m->path = файл архіву`, а [App_task_start](src/app/app_task.c#L79) робить `path_join(m->path, name)` → копіювання не працює |
| CRC / цілісність | `crc32_update` є, але ZIP CRC **не перевіряється**; tar checksum не перевіряється |
| tar | Лише `name[100]`: немає ustar `prefix`, GNU `L/K`, PAX `x/g`, base-256; не розрізняє symlink/hardlink; немає mode/mtime/uid |
| zip | Лише методи 0 і 8; немає UTF-8/CP437, 0x5455/0x7875, unix-атрибутів, Deflate64/bz2/lzma/zstd, детекції шифрування, префіксу SFX, перевірки перекриття записів |
| Інше | немає sniff за магією; немає `.gz/.bz2/.xz/.zst` single-file; `arc_populate` O(n²) (`DirModel_find_by_name` на кожен запис) |
| PAL | немає `futimens/utimensat`, `fchmod`(fd), `statfs`, `pwrite`, `geteuid`, `fallocate` ([duop_abi.h](include/duop_abi.h)) |
| Борг документації | [ARCHIVE_TOOLS_PLAN.md](docs/ARCHIVE_TOOLS_PLAN.md) і §1.1 [DEVELOPMENT_PLAN](docs/DEVELOPMENT_PLAN_2026-11-17.md) описують `.s`-модулі та `Alt+P`; ROADMAP §4.1 — невиконані чекбокси |

Спільні залежності: [pkg_archive.c](src/fs/pkg_archive.c) (пакетний менеджер) і [dir_search.c](src/fs/dir_search.c) (`ds_append_entry`) використовують `archive_parse.c`/`ArcEntry` — не ламати (compat-обгортка до фази 5). Існуючі тести: `test_c_archive.py` (диференційний vs frozen asm), `test_archive_vfs.py`, `test_archive_hardening.py`, `test_c_pkg_archive.py`.

## Цільова архітектура (усе freestanding C23, шар `src/fs/`, ≤500 рядків/файл, ≤16 експортів, функція ≤80 рядків, кадр ≤4608 Б)

**Ключова ідея:** архів завжди читається як **mmap-вхід** (`os_mmap_file_ro` + `SIGBUS`-guard через `os_try_begin`), декодери мають лише **потокову сторону виходу** (sink-callback, вікно 32 КіБ+, скасування кожні 64 КіБ). Це прибирає ліміти RAM і спрощує декодери (вхід цілий, без refill). Стиснений tar (`gz/bz2/xz/zst`) розпаковується **один раз у безіменний temp-файл** (fd створює UI до `fork`, worker пише в нього; після `unlink` файл не протікає навіть при `SIGKILL`), далі mmap → звичайний tar-індекс.

| Новий модуль | Відповідальність |
|---|---|
| `include/duop_codec.h` | спільний контракт: `ArcSink {write(ctx,buf,len), cancelled(ctx)}`, ліміти (`max_out`, `max_dict`), `-errno`/коди помилок формату |
| `src/fs/arc_source.c` | відкриття/mmap архіву, перевірка `dev/ino/size/mtime` («архів змінився»), sniff за магією, temp-fd для стиснутого tar |
| `src/fs/inflate_stream.c` | потоковий RFC 1951 (+Deflate64), CRC32 на льоту, null-sink; перевикористати таблиці/дерева з [inflate.c](src/fs/inflate.c) (`inflate_raw` лишається для PNG/пакетів) |
| `src/fs/gzip_dec.c` | RFC 1952, multi-member, CRC32+ISIZE |
| `src/fs/bzip2_dec.c` | BWT/MTF/Huffman, block CRC, multi-stream |
| `src/fs/xz_dec.c`, `lzma_dec.c` | xz-контейнер (CRC32/CRC64/SHA-256 check), LZMA2, BCJ-x86/ARM64/Delta; ліміт словника → зрозуміла помилка |
| `src/fs/zstd_dec.c`, `zstd_fse.c`, `zstd_huf.c` | кадри, FSE/Huffman, sequences, XXH64 (додати в [hash.c](src/core/hash.c)), skippable frames, `windowLog ≤ 27` |
| `src/fs/arc_tar.c`, `arc_zip.c` | парсери замість [archive_parse.c](src/fs/archive_parse.c) (залишається тонкою compat-обгорткою) |
| `src/fs/arc_index.c` | динамічний `mem_realloc` (перевірка OOM, стан не мутується до успіху), арена імен, **відсортований індекс** → листинг теки за `O(k log n)` |
| `src/fs/arc_path.c` | `arc_sanitize_relpath`, безпечний FD-relative walk (`openat O_NOFOLLOW\|O_DIRECTORY`) |
| `src/fs/archive_unpack.c`, `arc_unpack_meta.c` | розпаковування в `TaskWorker` (`TASK_OP_UNPACK`), метадані, конфлікти |
| `src/fs/archive_test.c` | `TASK_OP_ARC_TEST`: повний декод у null-sink, перевірка CRC/чексум, звіт |
| `src/fs/deflate.c`, `deflate_huff.c` | компресор: stored / fast (greedy+hash-chain) / normal (lazy), dynamic Huffman довжини ≤15, ≤1 МіБ RAM |
| `src/fs/tar_write.c`, `zip_write.c`, `archive_pack.c` | USTAR+PAX, gzip-обгортка, ZIP/ZIP64 (data descriptor, UTF-8 flag, 0x5455, unix-атрибути), обхід дерева, атомарний вивід |
| `src/app/app_arc_cmds.c` | UI-клей: Alt+F5/Alt+F9/F5, діалоги (не розростати `app_arc.c`/`dialog.c`) |
| `src/ui/pack_dialog.c` | формат (tar/tar.gz/zip), рівень (store/fast/normal), ім'я, «перевірити після пакування» |

**`ArcEntry` стає компактним** (зараз 288 Б із `name[256]`): `name_off`, `link_off`, `off`, `size`, `comp_size`, `mtime`, `mode`, `crc32`, `method`, `flags`(dir/symlink/hardlink/enc/utf8). Asm-посилань на цю структуру вже немає (0 не-PAL `.s`) — `_Static_assert` оновити; `pkg_*` мігрувати у фазі 5.

**PAL-доповнення** (`include/os/linux/{x86_64,aarch64}/pal_target.inc` + `pal_os.s`, без runtime-розгалужень): `os_futimens/utimensat`, `os_fchmod`, `os_fstatfs`, `os_pwrite`, `os_geteuid`, `os_fallocate` (best-effort); тест на обох ISA (`test_aarch64_full.py`/qemu).

## Безпека розпаковування (інваріанти)

- `arc_sanitize_relpath`: відкидає ведучі `/`, `.`/`..`-сегменти, NUL, перевіряє `PATH_MAX`; для zip з FAT-хоста `\`→`/` до перевірки.
- Створення лише через dirfd-relative `openat(O_NOFOLLOW)`; жодного слідування symlink, створених самим архівом (класична tar-атака symlink→запис).
- Symlink: лише якщо ціль не виходить за корінь розпаку (абсолютні/`..`-цілі відхиляються); hardlink — лише на вже розпакований запис; device/fifo — ніколи не створювати.
- Файл пишеться у `<ім'я>.duop-part` (`O_EXCL\|O_NOFOLLOW`) → перевірка розміру та CRC → `fsync` → `rename` (`RENAME_NOREPLACE`; конфлікт через існуючі [task_conflict.c](src/fs/task_conflict.c) `fo_resolve_eexist_open`: Overwrite/Skip/Rename/for-all). Збіг CRC/розміру ≠ → видалити part, `-EIO`.
- `mode & ~(S_ISUID\|S_ISGID\|S_ISVTX)`, `mtime` через `futimens`; метадані тек — відкладено до завершення вмісту; `chown` лише при `euid==0` і явній опції.
- Анти-bomb (`duop-bounded-resources`): pre-flight `fstatfs` проти суми розмірів зі сканування; ліміти кількості записів, глибини, ratio (≥1000:1 і >1 ГіБ → зупинка з повідомленням), максимуму розпакованого; перекриття записів у zip (quine-bomb) відхиляється; скасування `Esc` кожні 64 КіБ; усі ліміти мають локалізоване повідомлення.
- Шифрований zip (ZipCrypto/AES) — розпізнається і явно повідомляється «не підтримується» (не мовчазна помилка).

## Фази (кожна: тести першими → реалізація → `verify_build.sh` → bump `VERSION` → Conventional Commit)

**Фаза 0 — страхувальна сітка (v2.7.1, `fix`/`test`/`docs`)**
1. Тест-регресія «F5 в архіві» (Red) → тимчасово чисте повідомлення «архів лише для читання» замість зламаного копіювання; `Del/F6` у архіві — те саме.
2. Перевірити, що `Alt+F5/Alt+F9` доходять до диспетчера (`MOD_ALT` вже парситься в [input_seq.c](src/tui/input_seq.c); перевірити [app_keys.c](src/app/app_keys.c)).
3. `tests/archive_fixtures.py`: генератор корпусу (Python `tarfile/zipfile/gzip` + CLI `xz/bzip2/zstd/7z/zip`, які є на машині) — довгі імена, PAX, base-256, unicode, symlink, ZIP64, порожні, 100k записів, великі файли.
4. `tests/test_arc_fuzz.py` (детермінований seed: bit-flip/truncate/ZIP64 wrap) для існуючих парсерів; faultlab-проби для архіву.
5. Docs: замінити `ARCHIVE_TOOLS_PLAN.md` цим планом, виправити ROADMAP §4.1 та §1.1 DEVELOPMENT_PLAN.

**Фаза 1 — масштаб і потоковість (v2.8.0, `feat`)**
`arc_source` (mmap) → `arc_index` (динамічний, відсортований) → `inflate_stream` (+gzip) → tgz через unlinked temp-fd; `arc_tar` (ustar prefix, GNU L/K, PAX, base-256, symlink/hardlink, header checksum) і `arc_zip` (UTF-8/CP437, 0x5455/0x7875, unix-атрибути, SFX-префікс, узгодження local/central, детекція шифрування, перекриття). Прибрати `ARC_MAX_*` і прапорець `truncated`. Тести: `inflate_stream` ≡ `inflate_raw` (диференційно, вхід порізаний на всіх межах), архіви >6 МіБ/>100k записів під `MEMCHECK=1`, листинг теки з 100k записів ≪ 100 мс. Асинхронне відкриття великого tgz — через worker із прогрес-діалогом.

**Фаза 2 — розпаковування і тест (v2.8.1, `feat`)**
PAL-доповнення → `arc_path` + `archive_unpack` (`TASK_OP_UNPACK`, `TaskJob.src_dir`=архів, `names`=записи/префікси тек) → `F5` у архіві, `Alt+F9` на файлі-архіві (діалог: куди, «створити підтеку»; **smart-unpack**: якщо в корені >1 елемента — власна тека за іменем архіву) → `archive_test` (CRC без запису, звіт у StatusBar; пункт PopupMenu/Command Palette). `F3` великого запису (>8 МіБ) — через worker із прогресом. Тести: Zip-Slip (`../`, `/etc/passwd`), symlink-атака, пошкоджений CRC, ENOSPC/EIO (faultlab) → жодних `.duop-part`-залишків, `SIGKILL` посеред розпаковування, round-trip SHA-256 з еталоном (`tar`, `unzip`), mode/mtime, конфлікти.

**Фаза 3 — пакування (v2.8.2, `feat`)**
`deflate` (спершу stored+fixed Huffman, далі dynamic + lazy matching) → `tar_write` → gzip-обгортка → `zip_write` (ZIP64 автоматично) → `archive_pack` (TASK_OP_PACK, сортований обхід, `O_NOFOLLOW`, виключення самого вихідного архіву за `dev/ino`, файл зменшився/змінився під час читання → помилка + позначка) → `pack_dialog` + `Alt+F5`. Вивід атомарний (`.duop-part` → fsync → `RENAME_NOREPLACE`), «verify after pack» реюзає `archive_test`. Тести: round-trip через Python `tarfile/zipfile`, `unzip -t`, `tar -tzf`, `7z t`; відтворюваність (однаковий вхід → однаковий байт-в-байт вихід, `mtime` опційно); decompress нашим же `inflate_stream` ≡ вхід; ZIP64 >4 ГіБ на sparse-файлі.

**Фаза 4 — кодеки (v2.9.0, `feat`; кожен кодек окремим комітом)**
`bzip2_dec` → `xz_dec`/`lzma_dec` → `zstd_dec` (+XXH64). Підключення до: `.tar.bz2/.xz/.zst`, окремих `.bz2/.xz/.zst/.gz` (один віртуальний запис), zip-методів 9/12/14/93/95, sniff за магією (перейменовані файли). Тести: диференційно проти Python `bz2`/`lzma` та CLI `zstd` (Python 3.12 без `compression.zstd`), корпус зі стандартних тест-векторів, fuzz на кожен декодер (будь-який вхід → `-EINVAL`/обмежена пам'ять, ніколи не вихід за буфер), ліміт словника/window → зрозуміла помилка.

**Фаза 5 — завершення (v2.9.x, `feat`/`refactor`/`docs`)**
Міграція `pkg_archive.c` на новий API і видалення compat-обгортки; вкладені архіви (архів в архіві → temp → вхід, глибина ≤ 2); попередній перегляд записів у `F9`-панелі через частковий декод; інфо-рядок формату («ZIP · 1 234 файли · 56→12 МіБ»); конфіг-ключі лімітів; i18n-пакети; бенчмарк vs `tar/unzip/bsdtar` (доповнити [BENCHMARK](docs/BENCHMARK_20M_2026-11-17.md)-стиль); README/USER_GUIDE/HOTKEYS/ROADMAP.

## Наскрізні вимоги (кожна фаза)

- **TDD** (`duop-tdd-first`): тест у `tests/` (Red) до змін у `src/`; нові файли реєструються в `Makefile` `C_SRCS` і рядком у `test-serial:`; тести ≤500 рядків, `def` ≤100.
- **i18n:** кожен рядок — слот `LangStrings` ([types.inc](include/types.inc)) + `src/tui/i18n_en.lang` + `lang/uk.lang` + інші `lang/*.lang`; ширини через `utf8_display_width`. Нові `TASK_OP_*` — у [duop_task.h](include/duop_task.h) та `tests/test_abi_layouts.py`.
- **Надійність:** кожен `mem_alloc/realloc`/`os_*` перевіряється (`duop-error-path-contract`, `duop-oom-safety`); парсери під `TryFrame`; без рекурсії по дереву архіву (явний стек із лімітом); `tools/faultlab/campaign.py` + `audit_null_alloc_c.py` після змін у `src/fs`; SEU-перевірка CRC-таблиць декодерів (`duop-seu-hardening`).
- **Портативність:** жодного `libc`, VLA, `__int128`, float; тільки `load_le*`; кожен модуль збирається gcc (x86_64) і clang (aarch64); перевірка в qemu-aarch64 як в `test_c_archive.py` (AR4).
- **Гейти:** `check_lean.py`, `check_modularity.py`, `check_c_syntax.py`, `audit_elf_hardening.py`, `audit_live_regs.py`; `tests/test_c_stack_align.py` для нових PAL-викликів.
- **Реліз:** `bump_version.py` на кожну фазу, `build/version.inc`, Conventional Commit, оновлення ROADMAP/доків (`duop-docs-sync`).

## Верифікація (end-to-end)

1. `./.agents/skills/duop-pty-verify/scripts/verify_build.sh` (smart на проміжних кроках, `--full` перед тегом) і `make test`.
2. PTY-сценарії (`tests/test_pty.py` `PtySession`): відкрити 200 МіБ `.tar.zst`/`.zip` → навігація; `F5`/`Alt+F9` розпакувати → `diff -r` із еталоном; `Alt+F5` запакувати → `tar/unzip/7z/zstd -t` + розпакування еталонними утилітами; `Esc` посеред операції → жодних частин у цілі.
3. Fuzz/faultlab: `tests/test_arc_fuzz.py`, `tools/faultlab/campaign.py` (OOM, EIO, ENOSPC, SIGKILL), RSS-ліміт під `MEMCHECK=1` (архів 2 ГіБ ≈ <32 МіБ RSS).
4. Диференційні тести кожного декодера проти Python `zlib/bz2/lzma`, CLI `zstd`, `7z`.
5. Крос-арх: `test_aarch64_full.py` (qemu) на новому корпусі.

## Поза межами (свідомо)

`.rar` (пропрієтарний формат), `.7z`-запис і `.7z`-читання (можна додати після фази 4 поверх `lzma_dec`), запис/редагування «на місці» всередині існуючого архіву (add/delete/rename — окрема ініціатива), шифрування (ZipCrypto/AES), multi-volume, sparse-tar GNU (розпізнається й відхиляється з повідомленням).

## Ризики

- Обсяг декодерів (zstd/xz — найбільші): кожен декодер в окремому коміті з власним fuzz; фаза 4 не блокує релізи фаз 1–3.
- Міграція `ArcEntry` зачіпає `pkg_archive.c`/`dir_search.c` → compat-обгортка до фази 5, тести пакетного менеджера в кожному коміті.
- **Бюджет `include/types.inc` (≤400 рядків) вичерпано** (Фаза 0 втиснула слот `arc_readonly`, прибравши порожній рядок). Фази 2–3 додадуть десятки i18n-слотів → **передумова Фази 2**: винести `LangStrings` у `include/lang_strings.inc` (`.include` з `types.inc`) і оновити `tools/gen_i18n.py`, `check_modularity.py`, `.agents/hooks/duop_lifecycle.py`, `tests/test_c_i18n*.py`; також `LANG_SLOTS` ([duop_tui.h](../include/duop_tui.h)) та frozen-еталони `tests/harness/i18n_{table,keys}_reference.s` і число слотів у `test_c_i18n_table.py` правляться вручну на кожен новий слот.
- tgz/xz перший вхід вимагає повної декомпресії в temp (диск): обмежити розмір, показати прогрес, зрозуміла помилка при `ENOSPC`.
