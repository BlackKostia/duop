# Посібник розробника розширень та пакетів для Duop

Цей документ описує, як створювати, пакувати, тестувати та поширювати розширення для файлового менеджера **Duop** (актуально для `v2.5.3+`).

---

## 1. Архітектура та мови розробки

Duop — це статичний ELF64-бінарник (freestanding C23 поверх асемблерного PAL, `x86_64`/`aarch64`) без `libc` і без динамічного завантаження бібліотек (`dlopen`). Тому розширення працюють за моделлю **out-of-process** (окремий процес через `fork + execve`) або як **декларативні ресурсні пакети**.

Ви можете писати розширення **будь-якою мовою**:
- **Скриптові мови**: Python (`#!/usr/bin/env python3`), Bash/POSIX `sh` (`#!/bin/sh`), Node.js, Perl, Ruby тощо — достатньо вказати shebang у першому рядку.
- **Компілювані мови**: Rust, Go, C, C++, Zig, Assembly — скомпільований виконуваний бінарник.
- **Ресурсні пакети без коду**: мовні пакети (`*.lang`) та розкладки клавіатури (`*.layout`).

---

## 2. Типи пакетів (`kind`)

| `kind` | Призначення | Як активується в Duop |
|---|---|---|
| `tool` | Зовнішня CLI або TUI-утиліта | Запускається з **Менеджера пакетів (`Alt+K`, клавіша `r`)** та з **Меню користувача (`Alt+U`)**, якщо вказано `exec` / `menu`. |
| `lang` | Переклад інтерфейсу (`*.lang`) | Автоматично копіюється в `<pkg_root>/../lang/` і з'являється у перемиканні мов **`Alt+L`** та прапорці `--lang`. |
| `layout` | Розкладка клавіатури (`*.layout`) | Автоматично копіюється в `<pkg_root>/../layouts/` і підхоплюється для гарячих клавіш. |

---

## 3. Структура пакета та `manifest.ini`

### Правило структури архіву
Під час встановлення Duop розпаковує файли за **базовим іменем (`basename`)** у каталог `<pkg_root>/<name>/`.  
Тому всі файли пакета повинні лежати **у корені архіву `.tar` / `.tar.gz`** (без вкладених підпапок).

```text
my-ext-1.0.0.tar.gz
├── manifest.ini       # обов'язковий маніфест
├── my-tool            # головний виконуваний файл (для kind = tool)
└── helper_lib.py      # додаткові модулі в тій самій директорії
```

> **Примітка для Python/Node-пакетів:** імпортуйте сусідні модулі відносно директорії самого скрипта:
> ```python
> import os, sys
> sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
> ```

### Формат `manifest.ini`
Файл формату `key = value` (по одному параметру на рядок, пробіли навколо `=` обрізаються):

```ini
name = duop-sysmon
version = 1.0.0
kind = tool
description = Modern TrueColor system monitor (CPU, RAM/Swap, Disk, Top Processes)
exec = duop-sysmon
menu = System Monitor (CPU/RAM/Disk)
args = --cwd {dir}
scope = any
mode = tui
```

#### Опис полів маніфесту

| Поле | Макс. довжина | Обов'язкове | Опис |
|---|---|---|---|
| `name` | 63 байти | Так | Унікальне ім'я пакета (літери, цифри, `-`, `_`; **заборонено** `/` та `..`). |
| `version` | 31 байт | Так | Версія пакета (рекомендовано SemVer: `1.0.0`). |
| `kind` | 15 байтів | Так | Тип пакета: `tool`, `lang` або `layout`. |
| `description` | 111 байтів | Так | Короткий опис для `duop pkg list` та діалогу `Alt+K`. |
| `exec` | 63 байти | Для запуску | Ім'я виконуваного файлу всередині каталогу пакета (`<pkg_dir>/<exec>`). |
| `menu` | 63 байти | Ні | Назва пункту в **Меню користувача (`Alt+U`)**. Якщо не задано — пакет можна запустити з `Alt+K` (`r`), але він не засмічуватиме `Alt+U`. |
| `args` | 127 байтів | Ні | Аргументи запуску через пробіл/табуляцію з плейсхолдерами контексту. |
| `scope` | 15 байтів | Ні | Область дії (`any`, `file`, `dir`). |
| `mode` | 15 байтів | Ні | Режим виконання (`tui`). |

---

## 4. Контекст панелей, плейсхолдери `args` та безпека

У полі `args` можна використовувати плейсхолдери, які Duop підставляє перед запуском:

| Плейсхолдер | Значення |
|---|---|
| `{dir}` | Шлях до активної панелі Duop. |
| `{other_dir}` | Шлях до протилежної (неактивної) панелі. |
| `{file}` | Ім'я файлу/папки під курсором в активній панелі (або порожній рядок). |
| `{path}` | Повний шлях `<dir>/<file>` до елемента під курсором. |

### Як Duop виконує команду (Zero Shell-Injection)
1. Duop розбиває `args` на токени за пробілами/табами та формує прямий масив `argv[]` для системного виклику `execve` — **без виклику `/bin/sh -c`**.
   - Навіть якщо ім'я файлу містить пробіли або спецсимволи (наприклад, `hello world;rm -rf.txt`), воно передається в `argv[i]` як один безпечний рядок.
2. Перед запуском Duop автоматично гарантує права `0755` (`fchmodat`) на `<pkg_dir>/<exec>`.
3. Duop виводить термінал із raw-режиму (`term_exit_raw`), переходить у директорію активної панелі (`chdir`), запускає ваш процес і чекає на завершення (`wait4`).
4. Після виходу з утиліти Duop відновлює робочу директорію, повертає raw-режим, примусово перемальовує екран і перезавантажує обидві панелі (`App_reload_panes`) — тому будь-які створені чи змінені файли одразу видно в інтерфейсі.

---

## 5. Де зберігаються пакети

Коренева директорія пакетів (`<pkg_root>`) визначається за таким пріоритетом:
1. Змінна середовища `$DUOP_PKG_DIR`
2. `$XDG_CONFIG_HOME/duop/packages`
3. `$HOME/.config/duop/packages` (за замовчуванням)

Всередині `<pkg_root>/<name>/` Duop також зберігає службові файли:
- `sha256` — 64-символьний hex-дайджест встановленого архіву (використовується командою `verify`);
- `files.list` — список активованих файлів (наприклад, у `../lang/` чи `../layouts/`), щоб `duop pkg remove` міг чисто прибрати їх при видаленні.

---

## 6. Покроковий приклад: створення нового пакета-утиліти

Створимо мінімальне розширення `duop-wordcount`, яке рахує рядки, слова та байти виділеного файлу в сучасному TrueColor-стилі.

### Крок 1. Підготовка файлів
Створіть папку `packages/duop-wordcount/` з двома файлами:

**`manifest.ini`**:
```ini
name = duop-wordcount
version = 1.0.0
kind = tool
description = Count lines, words and bytes for selected file
exec = duop-wordcount
menu = Word Count (Selected File)
args = {path}
scope = file
mode = tui
```

**`duop-wordcount`** (скрипт):
```python
#!/usr/bin/env python3
import os
import sys

def main():
    if len(sys.argv) < 2 or not os.path.isfile(sys.argv[1]):
        print("\x1b[38;2;247;118;142mВиберіть звичайний файл перед запуском.\x1b[0m")
        input("Натисніть Enter...")
        return 1

    target = sys.argv[1]
    with open(target, "rb") as f:
        data = f.read()

    lines = data.count(b"\n")
    words = len(data.split())
    size = len(data)

    print(f"\x1b[38;2;122;162;247m╭─ Word Count ─────────────────────────────╮\x1b[0m")
    print(f"│ File : {os.path.basename(target)}")
    print(f"│ Lines: {lines}   Words: {words}   Bytes: {size}")
    print(f"\x1b[38;2;122;162;247m╰──────────────────────────────────────────╯\x1b[0m")
    input("Натисніть Enter для повернення в Duop...")
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
```

### Крок 2. Пакування в `.tar.gz` та генерація SHA-256
Використовуйте формат `USTAR` (`tarfile.USTAR_FORMAT` у Python або `tar --format=ustar`):

```python
import hashlib, os, tarfile

pkg_dir = "packages/duop-wordcount"
out_tar = "duop-wordcount-1.0.0.tar.gz"

with tarfile.open(out_tar, "w:gz", format=tarfile.USTAR_FORMAT) as tf:
    for fname, mode in (("manifest.ini", 0o644), ("duop-wordcount", 0o755)):
        ti = tf.gettarinfo(os.path.join(pkg_dir, fname), arcname=fname)
        ti.mode = mode
        with open(os.path.join(pkg_dir, fname), "rb") as f:
            tf.addfile(ti, f)

sha = hashlib.sha256(open(out_tar, "rb").read()).hexdigest()
print("Archive:", out_tar)
print("SHA-256:", sha)
```

> Живий приклад білдера в репозиторії: [build_sysmon_pkg.py](file:///home/chessplayer/DEV/NC/scripts/build_sysmon_pkg.py).

### Крок 3. Перевикористання готового UI-тулкіта
У складі `packages/duop-sysmon/` є готові модулі без зовнішніх залежностей, які можна копіювати у власні Python-пакети:
- [duop_tui_kit.py](file:///home/chessplayer/DEV/NC/packages/duop-sysmon/duop_tui_kit.py) — Tokyo Night TrueColor палітра (`Theme`), закруглені рамки (`draw_box`), градієнтні прогрес-бари (`progress_bar`), спарклайни (`sparkline`), Braille-графіки (`braille_graph`), менеджер термінала (`RawTerminal`).
- [sys_telemetry.py](file:///home/chessplayer/DEV/NC/packages/duop-sysmon/sys_telemetry.py) — збір телеметрії Linux з `/proc` і `statvfs` (`CpuSampler`, `read_memory`, `read_disks`, `ProcessSampler`).

---

## 7. Встановлення, запуск, перевірка та видалення

### Через CLI (`duop pkg`)
```bash
# Встановлення з локального архіву (рекомендовано із --sha256)
./duop pkg install duop-wordcount-1.0.0.tar.gz --sha256 <64-hex-sha256>

# Встановлення за HTTPS/HTTP URL (через системний curl або wget)
./duop pkg install https://example.com/duop-wordcount-1.0.0.tar.gz --sha256 <64-hex-sha256>

# Список встановлених пакетів
./duop pkg list

# Перевірка запису цілісності пакета
./duop pkg verify duop-wordcount

# Видалення пакета
./duop pkg remove duop-wordcount
```

### Через інтерфейс Duop (TUI)
- **`Alt+K`** — відкриває **Package Manager** із вкладками **`Installed`** / **`Available`**:
  - `Tab` — перемикання між вкладками **Installed** та **Available**;
  - `↑`/`↓` (`j`/`k`) — вибір пакета;
  - **`Enter`** або **`r`** — на вкладці **Installed** запускає `tool`-пакет (або перевіряє SHA-256, якщо немає `exec`); на вкладці **Available** встановлює вибраний пакет із реєстру;
  - `v` — перевірити SHA-256 запис;
  - `u` — оновити всі встановлені пакети за SemVer (**Installed**) або оновити індекс реєстру (**Available**);
  - `i` — встановити новий пакет за шляхом або URL;
  - `d` або `Delete` — видалити пакет;
  - `q` або `Esc` — закрити.
- **`Alt+U`** — відкриває **User Menu**, де автоматично відображаються всі пакети з полем `menu = ...` у `manifest.ini`.
- **`Ctrl+K`** — відкриває **Command Palette**, де також доступний нечіткий пошук і запуск встановлених розширень (`menu = ...`).
- **`Ctrl+G`** (`Go to`) — введіть `<scheme>://` (наприклад, `s3://` або `mockvfs://`) для інтерактивної навігації по VFS-провайдеру в панелі файлів (`Enter` входить у підкаталоги, `..` повертає назад, `F3` відкриває файл через `READ`).

---

## 8. Чекліст перед публікацією пакета

1. Усі файли мають безпечні відносні шляхи (без `/` на початку та без `..`; підтримуються підкаталоги `bin/`, `lib/`, `assets/`) у `.tar` / `.tar.gz` (формат `ustar`).
2. `manifest.ini` містить `name`, `version`, `kind`, `description` (і `exec` для `tool`).
3. Ім'я пакета (`name`) не містить `/` чи `..` і не перевищує 63 байти.
4. Головний скрипт/бінарник коректно обробляє вихід (`exit 0`) та відновлює термінал, якщо вмикав alternate screen (`\x1b[?1049l`).
5. Пакет перевірено на файлах із пробілами та спецсимволами в шляхах.
6. Для релізу згенеровано контрольну суму `sha256sum`.
