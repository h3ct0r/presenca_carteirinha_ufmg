# RFID Attendance Device (ESP32-P4 + LVGL)

<a href="https://youtube.com/shorts/oyJePKWmUqI?feature=share" target="_blank">
   <img src="https://raw.githubusercontent.com/h3ct0r/presenca_carteirinha_ufmg/refs/heads/main/assets/carteirinha_v0.1.jpg" alt="Watch the video" width="360" />
</a>

A classroom attendance device. Students register presence by tapping an RFID
card — or, in kiosk mode, by typing their university id. A portrait touchscreen
runs the UI (LVGL 9); an SD card holds all data (config, students, classes,
attendance); a buzzer gives audio feedback.

**Typical workflow:** a professor taps their card at the idle screen to unlock
the device → picks a class → opens a dated attendance session → takes roll call
(tap cards / tap names), enrolls students, or starts an unattended kiosk for
students to self-check-in. Attendance can later be exported to CSV.

## Hardware

Board: **Guition JC4880P443C** (module JC-ESP32P4-M3). Schematics and
datasheets are in [`docs/`](docs/) (schematic images in `docs/schematic/`).

- **MCU:** ESP32-P4 (no native WiFi/BT — WiFi runs on an external ESP32-C6).
- **Display:** ST7701, 480×800 portrait, RGB565 over MIPI-DSI.
- **Touch:** GT911 over I2C (polled; interrupt line not wired).
- **RFID:** PN532 over I2C (polled in a task).
- **Audio:** ES8311 codec → NS4150 amp → speaker (I2S).
- **Storage:** SD card (SD_MMC on the IOMUX pins).
- **Power:** IP5306 boost PMIC + TLV62569 buck. **SW3 is the power button** —
  on battery it must be pressed to boot.
- **No RTC:** session dates are chosen with a calendar picker.

## Build & test

```sh
pio run -e esp32p4          # build device firmware
pio test -e native          # run host unit tests
pio test -e native -f "native/test_roster"   # a single suite
```

`platformio.ini` has two environments: `esp32p4` (device) and `native` (host
tests). The native environment compiles only the hardware-free sources against
the mocks in `lib/hw_mocks/` (in-memory SD card, fake PN532, fake JPEG encoder,
pthread-backed FreeRTOS). Add every new hardware-free `.cpp` to that filter.

## Architecture

Strict layers; events flow up. Only `ui/` includes `lvgl.h`. Services own
hardware + SD and run FreeRTOS tasks, posting `app_event_t` to a single queue
that the LVGL thread drains.

```
ui/        LVGL screens / components / theme
app/       pure logic: event bus, auth, session, uid, roster types, battery curve
services/  own hardware + SD: config, roster, rfid, battery, export
storage/   SD modules: sd_card (mount), attendance_store, photo_store
drivers/   lcd/, touch/, rfid/, audio/
```

## Features

- **Idle access gate** — unlock by professor card or numeric password.
- **Classes** — the professor's class list, from the SD card.
- **Class** — Session (roll call / date picker), History (past sessions with
  attendance %), and Enroll (professor-locked) tabs.
- **Kiosk** — unattended student self-check-in; exit is professor-gated.
- **Admin panel** — profile, class list, and password set/change (written back
  to `config.json`).
- **CSV Export** — per-class attendance export to the SD card. See
  [`docs/software/EXPORT.md`](docs/software/EXPORT.md).

## SD card data

```
/config.json                 authorized professors (name, email, rfid_uid, numeric password)
/students/students.json      global student registry (id, name, rfid_uid)
/classes/<code>/class.json   class metadata + roster (references student ids)
/classes/<code>/attendance/YYYY-MM-DD.jsonl   append-only per-session logs
/csv_export/<code>.csv       CSV attendance export (MATRICULA,FREQ)
```

A sample card layout is in [`docs/software/sd_card_example/`](docs/software/sd_card_example/).

## Documentation

- [`docs/software/PROJECT_HANDOFF.md`](docs/software/PROJECT_HANDOFF.md) — architecture, screens,
  data model, gotchas, and the current backlog (start here to continue work).
- [`docs/software/EXPORT.md`](docs/software/EXPORT.md) — the CSV attendance export feature.
- [`docs/software/CUSTOM_FONT_GENERATION.md`](docs/software/CUSTOM_FONT_GENERATION.md) — building
  the custom Montserrat + FontAwesome fonts.
- [`docs/software/sd_card_example/`](docs/software/sd_card_example/) — sample SD card contents.
- [`test/README.md`](test/README.md) — the native test setup.

## License

[MIT](LICENSE) © 2026 Héctor Azpúrua.
