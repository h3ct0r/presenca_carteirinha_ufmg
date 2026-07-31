<div align="center">

# Presença Carteirinha UFMG

**A classroom attendance device — students register presence with a tap of their university RFID card.**

[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![Platform: ESP32-P4](https://img.shields.io/badge/Platform-ESP32--P4-E7352C?logo=espressif&logoColor=white)](https://www.espressif.com/en/products/socs/esp32-p4)
[![UI: LVGL 9](https://img.shields.io/badge/UI-LVGL%209-1D4ED8)](https://lvgl.io)
[![Build: PlatformIO](https://img.shields.io/badge/Build-PlatformIO-FF7F00?logo=platformio&logoColor=white)](https://platformio.org)

<a href="https://youtube.com/shorts/oyJePKWmUqI?feature=share" target="_blank">
   <img src="https://raw.githubusercontent.com/verlab/presenca_carteirinha_ufmg/refs/heads/main/assets/carteirinha_v0.1.jpg" alt="Watch the demo video" width="380" />
</a>

<sub>▶ Click to watch the demo</sub>

[Documentation](#documentation) · [Architecture](docs/software/ARCHITECTURE.md) · [Config import](docs/software/CONFIG_IMPORT.md) · [Backlog](docs/software/BACKLOG.md)

</div>

---

Students register presence by tapping an RFID card — or, in kiosk mode, by typing
their university id. A portrait touchscreen runs the UI (LVGL 9); an SD card
holds all data; a buzzer gives audio feedback.

**Typical workflow:** a professor taps their card at the idle screen to unlock
the device → picks a class → opens a dated attendance session → takes roll call
(tap cards / tap names), enrolls students, or starts an unattended kiosk for
students to self-check-in. Attendance exports to CSV.

Classes and students are authored in the browser
[**Config builder**](https://verlab.github.io/presenca_carteirinha_ufmg/config/)
(source: [`tools/config-builder/`](tools/config-builder/)) and imported as a
`config.tar`; the device never needs a keyboard for data entry.

## Hardware

Board: **Guition JC4880P443C** (module JC-ESP32P4-M3). Schematics and datasheets
are in [`docs/hardware/`](docs/hardware/).

- **MCU:** ESP32-P4 (no native WiFi/BT — WiFi runs on an external ESP32-C6).
- **Display:** ST7701, 480×800 portrait, RGB565 over MIPI-DSI.
- **Touch:** GT911 over I2C (polled; interrupt line not wired).
- **RFID:** PN532 over I2C (polled in a task).
- **Camera:** OV02C10 2MP over MIPI-CSI, with ESP-DL face detection.
- **Audio:** ES8311 codec → NS4150 amp → speaker (I2S).
- **Storage:** SD card (SD_MMC on the IOMUX pins).
- **Power:** IP5306 boost PMIC + TLV62569 buck. **SW3 is the power button** —
  on battery it must be pressed to boot.
- **No RTC:** session dates are chosen with a calendar picker.

## Web tools (GitHub Pages)

**[verlab.github.io/presenca_carteirinha_ufmg](https://verlab.github.io/presenca_carteirinha_ufmg/)**

| Tab | URL | What it does |
|-----|-----|----------------|
| Config | [/config/](https://verlab.github.io/presenca_carteirinha_ufmg/config/) | Author `config.tar` (teachers, students, classes, photos) |
| Firmware | [/firmware/](https://verlab.github.io/presenca_carteirinha_ufmg/firmware/) | USB flash (Chrome / Edge, Web Serial) |

Download `config.tar` → place at SD root `/config.tar` (card reader or device
Wi‑Fi **SD File Manager**) → import on device. Tag `v*` → GitHub Release bins →
Pages syncs them for the installer. See [`docs/site/`](docs/site/) and
[`docs/flasher/DEPLOY.md`](docs/flasher/DEPLOY.md).

## Build & test

```sh
pio run -e esp32p4          # build device firmware
pio test -e native          # run host unit tests
```

`platformio.ini` has two environments: `esp32p4` (device) and `native` (host
tests against the mocks in `lib/hw_mocks/`). See [`test/README.md`](test/README.md).

## Features

- **Idle access gate** — unlock by professor card or numeric password.
- **Classes** — the professor's class list, read from the SD card.
- **Class** — a hub opening into a dated roll-call session, past-session history
  with attendance %, and student enrollment. Per-class settings and statistics
  live behind the ⚙ button.
- **Check-in modes**, per class — single tap, double tap (arrival + confirm after
  a threshold), or photo check-in with face verification.
- **Kiosk** — unattended student self-check-in; exit is professor-gated.
- **Student avatars** — imported photos shown on every check-in.
- **CSV export** — per-class attendance to the SD card.
- **Config import** — apply a `config.tar` with backup and rollback.
- **WiFi file manager** — a soft-AP serving a browser file manager for the card
  (list, edit, upload by drag-and-drop, rename, delete). Debug tool, no auth.
- **Admin** — profile, SD usage, password, professor card binding, camera
  preview, config import, and destructive debug wipes.

## Documentation

**Start here**
- [Architecture](docs/software/ARCHITECTURE.md) — layers, threading, screens, and
  the LVGL gotchas that have caused real bugs.
- [SD card layout](docs/software/SD_CARD.md) — every path the device reads or writes.
- [Backlog](docs/software/BACKLOG.md) — known-open work.

**Features**
- [Config import](docs/software/CONFIG_IMPORT.md) — the authoritative `config.tar`
  format, schemas and limits. Firmware and the config-builder both follow it.
- [CSV export](docs/software/EXPORT.md)
- [Photo check-in](docs/software/FACE_CHECKIN.md) — face-verified kiosk check-in.
- [Face detection](docs/software/FACE_DETECTION.md) — the camera + ESP-DL stack.
- [Student photos](docs/software/STUDENT_PHOTOS.md) — the avatar pipeline, from
  Moodle export to on-screen.

**How-to**
- [Decoding a Guru Meditation panic](docs/software/DEBUGGING_PANICS.md) — turning a
  RISC-V register dump into `file.cpp:line` with `riscv32-esp-elf-addr2line`.
- [Downloading student photos from Moodle](docs/software/MOODLE_PHOTOS.md)
- [Generating the custom fonts](docs/software/CUSTOM_FONT_GENERATION.md)
- [Sample SD card](docs/software/sd_card_example/) — a complete card to copy.
- [Web firmware installer](docs/flasher/) — browser USB flash (GitHub Pages).

**Tools**
- [config-builder](tools/config-builder/README.md) — the offline browser tool that
  authors `config.tar`. Also has a [spec](tools/config-builder/SPEC.md) and
  [deployment notes](tools/config-builder/DEPLOY.md).
- [Serving the builder from the device](docs/software/ONBOARD_CONFIG_BUILDER.md) —
  designed, not built.

**Contributing**
- [`CLAUDE.md`](CLAUDE.md) — build/test commands, layer rules, and the conventions
  agents and contributors follow.
- [`test/README.md`](test/README.md) — the native test setup and suite list.

## Authors

- **Prof. Héctor Azpúrua**
- **Prof. Paulo Rezeck**
- **Prof. Douglas G. Macharet**
- **Aline Molinar**

Computer Science Department (DCC) — Universidade Federal de Minas Gerais.

## Acknowledgements

<div align="center">
   <img src="assets/verlab.jpg" alt="VeRLab" height="72" />
   &nbsp;&nbsp;&nbsp;&nbsp;
   <img src="assets/GEAR.jpg" alt="GEAR" height="72" />
</div>

Developed with the support of **VeRLab** (Laboratory of Computer Vision and
Robotics) and **GEAR** (Grupo de Estudos Avançados em Robótica).

## Built with

| Project | License |
|---|---|
| [LVGL](https://lvgl.io) | MIT |
| [Montserrat](https://fonts.google.com/specimen/Montserrat) | SIL OFL 1.1 |
| [Font Awesome Free](https://fontawesome.com) | CC BY 4.0 |
| [ESP-DL](https://github.com/espressif/esp-dl) | Apache 2.0 |

## License

[MIT](LICENSE) © 2026 Héctor Azpúrua.
