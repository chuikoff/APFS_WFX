# Total Commander macOS APFS File System Plugin (WFX)

[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![Platform](https://img.shields.io/badge/Platform-Windows%20(x64%20%7C%20x86)-lightgrey.svg)]()
[![Total Commander](https://img.shields.io/badge/Total%20Commander-WFX%20Plugin-green.svg)]()

WFX-плагин для **Total Commander** (поддерживает как **64-bit**, так и **32-bit** версии) для прямого чтения и извлечения файлов с физических накопителей (USB-флешек, внешних SSD/HDD), отформатированных в файловой системе Apple **APFS** (macOS High Sierra — Sequoia).

---

## 🚀 Возможности

- **Прямой доступ к накопителям**: Сканирует физические диски Windows `\\.\PhysicalDriveX` и находит GPT-разделы с GUID контейнеров APFS (`7C3457EF-0000-11AA-AA11-00306543ECAC`), а также контейнеры на неразмеченных дисках.
- **Поддержка томов APFS**: Внутри одного APFS-контейнера может находиться несколько томов (*Macintosh HD*, *macOS — данные*, *Data*, *Preboot*, *Recovery* и т.д.) — плагин отображает каждый том как отдельный каталог.
- **Дерево папок и файлы**: Полноценная навигация по подкаталогам, корректная поддержка русских и любых Unicode-имен файлов, атрибутов (`Read-Only`), точных размеров и меток времени создания/модификации.
- **Встроенная компрессия (Decmpfs)**: Прозрачная декомпрессия файлов macOS, сжатых алгоритмами LZVN, LZFSE и ZLIB.
- **Быстрый просмотр эскизов (Thumbnails)**:
  - Мгновенная генерация эскизов в памяти (GDI+) для JPG, PNG, BMP, GIF, ICO, TIFF без лишних операций ввода-вывода.
  - Интеграция с Total Commander для просмотра HEIC, WebP, PDF и видеофайлов.
  - Поддержка горячих клавиш `Ctrl + Shift + F1` (режим «Эскизы»).
- **Собственные значки (Custom Icons)**: Фирменная иконка дисков Apple/APFS в «Сетевом окружении» и списке накопителей.
- **Информационный пункт «О программе»**: Быстрый вызов через пункт `[О программе - Mac APFS].txt` или по `Alt + Enter` (Свойства).
- **Операции Total Commander**:
  - `F3` — быстрый просмотр файла (Lister);
  - `F5` — копирование файлов и папок с Mac-накопителя на диск Windows;
  - `Enter` — запуск ассоциированной программы Windows (файл автоматически загружается во временную папку и открывается);
  - `Ctrl + R` — обновление списка подключенных накопителей.
- **Безопасность данных**: Работает строго в режиме **Read-Only** (только чтение). Никакие изменения на физический диск не вносятся.

---

## 📦 Быстрая установка в Total Commander

1. Скачайте готовый архив [`tc_apfs.zip`](https://github.com/chuikoff/APFS_WFX/releases/latest).
2. Запустите **Total Commander от имени Администратора** (это обязательно для прямого доступа к секторам `\\.\PhysicalDriveX`).
3. В панели Total Commander откройте файл `tc_apfs.zip` (нажмите `Enter`).
4. Total Commander автоматически определит плагин файловой системы и предложит установить его. Нажмите **«Да»**.

### Ручная установка:
1. Меню **Конфигурация** -> **Настройка...** -> **Плагины**.
2. В блоке **Плагины файловой системы (.WFX)** нажмите кнопку **Настройка**.
3. Нажмите **Добавить** и укажите `apfs.wfx64` (для 64-битного TC) или `apfs.wfx` (для 32-битного TC).
4. Задайте имя виртуального диска, например: `Mac APFS Drives`.

---

## 🖥 Как пользоваться

1. Подключите USB-флешку или внешний SSD с файловой системой APFS к компьютеру.
2. Откройте Total Commander (от имени Администратора).
3. В списке дисков перейдите в **«Сетевое окружение»** (Network Neighborhood).
4. Войдите в папку **«Mac APFS Drives»**.
5. Вы увидите список обнаруженных дисков, например:
   ```text
   \--- [О программе - Mac APFS].txt
   \--- Drive 1 - [USB] SanDisk Extreme (64.0 GB)
        \--- macOS — данные
             +--- Applications
             +--- Users
             |    \--- Alex
             |         +--- Documents
             |         \--- Photos
             \--- Library
   ```
6. Выбирайте файлы, просматривайте по `F3`, включайте показ эскизов по `Ctrl + Shift + F1` или копируйте по `F5`.

---

## 🛠 Архитектура и сборка из исходников

Проект собран на C++17 с использованием MSVC (Visual Studio 2022).

- `include/fsplugin.h` — WFX API спецификация Total Commander.
- `src/plugin_main.cpp` — экспортируемые точки входа (`FsInitW`, `FsFindFirstW`, `FsFindNextW`, `FsGetFileW`, `FsExecuteFileW`, `FsGetPreviewBitmapW`, `FsExtractCustomIconW`).
- `src/drive_enumerator.cpp` — поиск физических дисков Windows, проверка GPT-таблицы разделов на наличие GUID контейнера APFS.
- `src/apfs_vfs.cpp` — виртуальная файловая система, кэширование открытых контейнеров и томов, декодирование эскизов GDI+, диалог «О программе».
- `res/` — ресурсы иконки Apple/APFS и версия сборки.
- `apfs-fuse-src/ApfsLib` — парсер структур APFS (B-деревья, Object Map, Inodes, Decmpfs).

### Сборка:
```powershell
# 64-bit (x64)
cmake -B build -S . -A x64
cmake --build build --config Release

# 32-bit (x86)
cmake -B build32 -S . -A Win32
cmake --build build32 --config Release
```

Готовые бинарные файлы будут созданы в `build/Release/apfs.wfx64` и `build32/Release/apfs.wfx`.

---

## 📄 Лицензия

MIT License — подробности в файле [LICENSE](LICENSE).
