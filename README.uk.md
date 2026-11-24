# Duop (`duop`)

[![CI](https://github.com/BlackKostia/duop/actions/workflows/ci.yml/badge.svg)](https://github.com/BlackKostia/duop/actions/workflows/ci.yml)
[![Release](https://img.shields.io/github/v/release/BlackKostia/duop?label=release)](https://github.com/BlackKostia/duop/releases/latest)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![Static ELF](https://img.shields.io/badge/static_ELF-249_KiB_(x86__64)_%7C_285_KiB_(aarch64)-success)](#порівняння-та-бенчмарки)

[English](README.md) | **Українська**

**Ультрашвидкий двопанельний файловий менеджер для термінала Linux (`x86_64` та `aarch64`) розміром ~250 КіБ без `libc`.**
Єдиний статичний ELF-бінарник — без `libc`, без `ncurses`, без жодних зовнішніх залежностей. Відкриває каталог на 4,8 мільйона файлів за **26 мс** (15,4 МіБ RSS) і містить нечіткий пошук, піксельне прев'ю зображень, git-статуси, hex-редактор, інспектор ELF64, архівну VFS (розпакування й пакування) та вбудований PTY-термінал прямо з коробки.

![Duop — український інтерфейс (тема Tokyo Night)](docs/media/ukrainian.svg)

<details>
<summary><strong>Більше скриншотів: Live Preview, Палітра команд, Тема Catppuccin, Англійський інтерфейс</strong></summary>

#### Живий перегляд коду з підсвіткою синтаксису (`F9`)
![Duop live preview](docs/media/preview.svg)

#### Палітра команд (`Ctrl+K`)
![Duop command palette](docs/media/palette.svg)

#### Тема Catppuccin (`Alt+T`) та Англійський інтерфейс (`Alt+L`)
| Тема Catppuccin | English UI (`--lang en`) |
| --- | --- |
| ![Catppuccin](docs/media/theme.svg) | ![English UI](docs/media/main.svg) |

*(Усі скриншоти генеруються детерміновано з реальної PTY-сесії скриптом [`tools/screenshot_svg.py`](tools/screenshot_svg.py).)*
</details>

---

## Встановлення

### Встановлення однією командою (готовий статичний бінарник + перевірка SHA-256, або збірка з коду)

```bash
curl -fsSL https://raw.githubusercontent.com/BlackKostia/duop/main/scripts/install.sh | bash
```

### Готові статичні бінарники (`x86_64` та `aarch64`)

Завантажте `duop-linux-x86_64.tar.gz` або `duop-linux-aarch64.tar.gz` (разом із `SHA256SUMS`) зі сторінки [**GitHub Releases**](https://github.com/BlackKostia/duop/releases/latest).

### Пакетні менеджери

| Платформа | Команда / Рецепт |
| --- | --- |
| **Arch Linux (AUR)** | `makepkg -si` у каталозі [`packaging/aur/`](packaging/aur/PKGBUILD) |
| **Debian / Ubuntu (`.deb`)** | `./packaging/deb/build-deb.sh && sudo dpkg -i build/deb/duop_*.deb` |
| **Nix (Flakes)** | `nix run github:BlackKostia/duop` або `nix profile install github:BlackKostia/duop` |
| **Homebrew (Linux)** | `brew install --build-from-source ./Formula/duop.rb` |

### Збірка з вихідного коду та тестування

Потрібні `binutils`, `make`, `python3` та `gcc >= 13` (на `x86_64`) або `clang` (на `aarch64`):

```bash
make                        # Зібрати ./duop для Linux x86-64 (з DWARF debug-інформацією)
make release                # Зібрати оптимізований stripped-бінарник x86-64 (249 КіБ)
make ARCH=aarch64 release   # Крос-компіляція статичного бінарника Linux AArch64 (285 КіБ)
make test                   # Запустити всі 204 тестові сюїти (PTY, диференційні, AArch64, ФС)
make verify                 # Повна перевірка (модульність + release + debug + аудит ELF + PTY)
make hooks                  # Активувати Git-хуки з .githooks/
```

---

## Порівняння та бенчмарки

| Характеристика / Метрика | **Duop (`v2.11.0`)** | `mc` | `yazi` | `lf` | `nnn` |
| --- | --- | --- | --- | --- | --- |
| **Розмір бінарника (`x86_64`)** | **249 КіБ** (статичний) | ~1,4 МіБ + бібліотеки | ~12–20 МіБ | ~4,5 МіБ | ~120 КіБ + `libc`/`ncurses` |
| **Зовнішні залежності** | **0 (zero-`libc` static ELF)** | `libc`, `glib2`, `ncurses` | `libc` (+ CLI-утиліти) | `libc` | `libc`, `ncurses` |
| **Компонування** | **2 панелі + Вкладки + Прев'ю** | 2 панелі | Колонки Міллера | Колонки Міллера | 1 панель |
| **Каталог на 4 798 976 файлів (перший кадр)** | **26,4 мс** (15,4 МіБ RSS) | > 60 с (таймаут) | > 60 с (таймаут) | > 60 с (таймаут) | > 60 с (таймаут) |
| **Каталог на 100 000 файлів (перший кадр)** | **112,7 мс** | ~1,5–3 с | ~0,4–0,9 с | ~0,5–1,2 с | ~0,25–0,5 с |
| **Холодний старт (`--version`, `hyperfine -N`)** | **1,3 мс** (`/bin/true`: 1,0 мс) | 9,9 мс | ~6–12 мс | ~4–7 мс | ~2,8 мс |
| **Вбудоване прев'ю зображень (без утиліт)** | **Kitty / Sixel / блоки** | Зовнішнє | Вбудоване + плагіни | Зовнішнє | Плагіни |
| **Вбудований Hex-редактор та інспектор ELF64** | **Так (із Undo)** | Лише перегляд | Зовнішній | Зовнішній | Зовнішній |
| **Архіви: перегляд / розпакування / створення** | **Так** (`.tar`, `.zip`, `.gz`/`.xz`/`.bz2`/`.zst` + `.tar.*`; пакування в `.tar`/`.tar.gz`/`.zip`) | Повна VFS | Через `unar`/`7z` | Через скрипти | Через плагіни |

Рядки про каталоги на 4,8 млн і 100 000 файлів виміряно на `v2.7.0`, розмір і холодний старт — на `v2.11.0`. Повний звіт порівняльного тестування на 4,8 млн файлів: [`docs/BENCHMARK_20M_2026-11-17.md`](docs/BENCHMARK_20M_2026-11-17.md). Локальний прогін: `./scripts/bench.sh ./duop`.

---

## Чому гібрид (Asm PAL + Freestanding C23)

> **Архітектура та статус платформ:** Поетапну міграцію з чистого x86-64 GNU Assembler на гібрид **Asm PAL + freestanding `no-libc` C23** ([HYBRID_PORTABILITY_PLAN.md](docs/HYBRID_PORTABILITY_PLAN.md)) **завершено** (`v2.6.90+`, поза `src/os/` не лишилося жодного `.s`-файлу). І **Linux x86-64**, і **Linux AArch64** збирають повний статичний ELF64-бінарник `duop` (`.data = 0`, NX-safe), перевірений **186 тестовими сюїтами** включно з реальними PTY-сесіями на x86-64 та під `qemu-aarch64`.

- **Мінімальний статичний ELF64 без `libc`**: у C23 використовується режим `-ffreestanding -nostdlib` (лише компіляторні заголовки `stdint.h`/`stddef.h` та `include/duop_abi.h`), без динамічних чи сторонніх бібліотек.
- **Низькорівнева швидкість там, де вона критична**: прямі системні виклики, `io_uring`, обробники сигналів (`SIGBUS`), PTY та SSE2/NEON SIMD залишаються на чистому асемблері у `src/os/<os>/<arch>/` (`28` файлів `.s` під `201` файлом `.c`).

### Ієрархія класів (vtable-ООП)

```text
Object
 ├── ScreenBuffer    (подвійний буфер термінала, TrueColor RGB diff-рендер)
 ├── DirModel        (компактні 32Б записи + name_pool, лінивий/io_uring stat, потокове читання, Introsort + LSD Radix Sort, ls -f режим)
 ├── App             (головний цикл подій, диспетчеризація клавіатури/миші, файлові команди)
 └── Widget
       ├── HeaderBar     (верхня панель, індикатори режиму, клікабельні кнопки Theme/Preview/Dot/Help/Quit)
       ├── FunctionBar   (нижня панель кнопок 1..0 з hover-підсвічуванням і кліком)
       ├── StatusBar     (таймерні статус-сповіщення, деталі файлу, ціль symlink)
       ├── FilePane      (файлова панель, скролбар, сортування кліком по шапці, double-click)
       ├── PreviewPane   (живий попередній перегляд коду, зображень, ELF та hex-дампу)
       └── Dialog        (ConfirmDialog, InputDialog, ViewerDialog, HelpDialog, ProgressDialog, Fuzzy/Palette/Package/BulkRename...)
```

## Керування

### Клавіатура
- `↑` / `↓`, `j` / `k`, `PgUp` / `PgDn`, `Home` / `End` — навігація по списку; `Tab` — перемикання панелі; `Enter` — увійти в теку / відкрити файл; `Backspace` / `~` — на рівень вище / у `$HOME`
- `Space` / `Insert` / `*` / `Ctrl+A` — позначити файл або перемкнути виділення всіх; `/` — швидкий фільтр; `.` або `Ctrl+H` — приховані файли
- `Alt+T` — перемикання теми (`Tokyo Night`, `Catppuccin`, `Emerald`); `Alt+L` — наступна мова інтерфейсу (`*.lang` пакети без перекомпіляції; працюють у будь-якій розкладці завдяки `layouts/*.layout`)
- `F1` — довідка, `F2` — перейменування, `F3` — перегляд (Текст / HEX / Зображення: Kitty, Sixel, блоки 2×2 для PNG, JPEG, GIF, BMP, TGA, PNM, QOI), `F4` — зовнішній `$EDITOR`, `F5` — копіювання, `F6` — переміщення, `F7` — створити каталог, `F8` / `Del` — видалення (XDG Trash + `Ctrl+Z` Undo), `F9` / `Ctrl+P` — Live Preview, `F10` / `q` — вихід
- Архіви: `Enter` на `.tar`/`.zip`/`.gz`/`.xz`/`.bz2`/`.zst`/`.tar.*` відкриває їх як теку (вид визначається за вмістом); `F5` в архіві або `Alt+F9` на архіві — розпакувати; `Alt+Shift+F9` — перевірити; `Alt+F5` — запакувати позначене в `.tar`/`.tar.gz`/`.zip`
- `Ctrl+K` — палітра команд, `Alt+P` — рекурсивний Fuzzy Finder, `Ctrl+F` — рекурсивний пошук/grep у віртуальну панель, `Ctrl+T` / `Ctrl+W` — вкладки, `Ctrl+D` / `z` — закладки та frecency, `Ctrl+\`` — вбудований PTY-термінал, `Alt+D` — аналіз диска, `Alt+B` — масове перейменування, `Alt+=` — порівняння панелей, `Alt+K` / `Alt+U` — менеджер пакетів та меню користувача (повний перелік у [`docs/HOTKEYS.md`](docs/HOTKEYS.md))

### Миша (SGR 1006)
- Лівий клік / подвійний клік / перетягування (drag-and-drop між панелями), правий клік (контекстне меню або позначення), колесо прокрутки, сортування кліком по заголовках колонок (`Name`, `Size`, `Modified`), перетягування скролбару.

## Документація та участь у розробці

- Посібник користувача та гарячі клавіші: [`docs/USER_GUIDE.md`](docs/USER_GUIDE.md), [`docs/HOTKEYS.md`](docs/HOTKEYS.md), [`docs/EXTENSIONS_GUIDE.md`](docs/EXTENSIONS_GUIDE.md)
- Дорожня карта та продуктова стратегія: [`ROADMAP.md`](ROADMAP.md), [`docs/PRODUCT_STRATEGY_2026-11-20.md`](docs/PRODUCT_STRATEGY_2026-11-20.md), [`docs/DEVELOPMENT_PLAN_2026-11-17.md`](docs/DEVELOPMENT_PLAN_2026-11-17.md)
- Як долучитися (переклади `*.lang`, теми, пакети, C23-модулі): [`CONTRIBUTING.md`](CONTRIBUTING.md)

## Розробник та ліцензія

- **Автор**: Недбайло Костянтин Геннадійович (`Kostiantyn Nedbailo`) — [ChessPlayerSedoy@gmail.com](mailto:ChessPlayerSedoy@gmail.com)
- **Ліцензія**: [**MIT License**](LICENSE)
