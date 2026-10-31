# Total Commander macOS File System Plugin (WFX) — APFS & HFS+

[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![Platform](https://img.shields.io/badge/Platform-Windows%20(x64%20%7C%20x86)-lightgrey.svg)]()
[![Total Commander](https://img.shields.io/badge/Total%20Commander-WFX%20Plugin-green.svg)]()

WFX-плагин для **Total Commander** (поддерживает как **64-bit**, так и **32-bit** версии) для прямого чтения и извлечения файлов с физических накопителей (USB-флешек, внешних SSD/HDD), отформатированных в файловых системах macOS:
- **Apple APFS** (macOS High Sierra — Sequoia);
- **HFS+ / Mac OS Extended** (macOS Yosemite — Sequoia).

---

## 🚀 Возможности

- **Прямой доступ к накопителям**: Сканирует физические диски Windows `\\.\PhysicalDriveX` и находит GPT и MBR разделы с APFS (`7C3457EF-0000-11AA-AA11-00306543ECAC`) и HFS+ (`48465300-0000-11AA-AA11-00306543ECAC`), а также накопители без разметки.
- **Поддержка томов APFS и HFS+**: Отображает каждый том накопителя (*Macintosh HD*, *macOS — данные*, *Data*, *Preboot*, *Recovery*, HFS+ разделы) как отдельный каталог.


## 📄 Лицензия

MIT License — подробности в файле [LICENSE](LICENSE).
