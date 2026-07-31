# RFID Attendance Device (ESP32-P4 + LVGL)

<a href="https://youtube.com/shorts/oyJePKWmUqI?feature=share" target="_blank">
   <img src="https://raw.githubusercontent.com/h3ct0r/presenca_carteirinha_ufmg/refs/heads/main/assets/carteirinha_v0.1.jpg" alt="Watch the video" width="360" />
</a>

A classroom attendance device. Students register presence by tapping an RFID
card — or, in kiosk mode, by typing their university id. A portrait touchscreen
runs the UI (LVGL 9); an SD card holds all data; a buzzer gives audio feedback.

**Typical workflow:** a professor taps their card at the idle screen to unlock
the device → picks a class → opens a dated attendance session → takes roll call
(tap cards / tap names), enrolls students, or starts an unattended kiosk for
students to self-check-in. Attendance exports to CSV.

Classes and students are authored on a laptop with
[`tools/config-builder/`](tools/config-builder/) and imported as a `config.tar`;
the device never needs a keyboard for data entry.

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
- [Downloading student photos from Moodle](docs/software/MOODLE_PHOTOS.md)
- [Generating the custom fonts](docs/software/CUSTOM_FONT_GENERATION.md)
- [Sample SD card](docs/software/sd_card_example/) — a complete card to copy.

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

## License

[MIT](LICENSE) © 2026 Héctor Azpúrua.
