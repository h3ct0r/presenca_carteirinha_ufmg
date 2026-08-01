# CLAUDE.md — RFID Attendance Device (ESP32-P4 + LVGL)

Classroom attendance device: students tap an RFID card (or type their ID in
kiosk mode) to register presence. LVGL 9 touchscreen UI; all data on an SD card.
See [`docs/software/ARCHITECTURE.md`](docs/software/ARCHITECTURE.md) for the full
picture and [`docs/software/BACKLOG.md`](docs/software/BACKLOG.md) for open work —
this file is the always-loaded short version. The doc index is in the
[README](README.md).

## Build & test (run BOTH after every change)

```sh
pio run -e esp32p4                             # build device firmware
pio test -e native                             # host unit tests (all suites)
pio test -e native -f "native/test_roster"     # one suite
```

- `esp32p4` = device firmware (compiles all of `src/`).
- `native` = hardware-free host tests via `build_src_filter` against `lib/hw_mocks/`.
- **Add every new hardware-free `.cpp` to the native `build_src_filter`** in
  `platformio.ini`, or it won't be tested.
- **Optional build flags live commented out in `platformio.ini`** (currently
  `BATTERY_DRAIN_LOG`). Code they gate is still compiled by the native env, but
  the default device build never sees it — **build both ways** when you touch
  it: `PLATFORMIO_BUILD_FLAGS="-D BATTERY_DRAIN_LOG" pio run -e esp32p4`.

## Hardware testing (important)

**The maintainer manually tests every feature on the real device before starting
the next one.** Each change is flashed and exercised by hand on hardware as part
of the development loop, so shipped features here are device-verified, not just
build-verified, and behave as expected.

What this means for you:
- **Still report what YOU actually did.** You can only run `pio run` and
  `pio test`, so describe your own work as **"build- and native-test-verified"**
  and say plainly that you have not run it on hardware. Never claim a device
  check you didn't perform, and never infer one from this section.
- **Still flag the device-only surfaces** — LVGL rendering/crashes, camera,
  WiFi, SD timing, stack overflows — since they surface only when flashed. The
  difference is that an on-device check reliably *happens* here; it is the
  maintainer's step, not an open risk you should describe as unlikely to occur.
- **Don't rewrite history.** Changelog entries that say "not run on hardware"
  recorded the truth at the time; leave them.

## Architecture — strict layers, events flow up

```
src/ui/        LVGL screens/components/theme — the ONLY code that includes lvgl.h
src/app/       pure logic, no hardware: event_bus, auth, session, uid, ustar,
               battery_curve, photo_fit — this is what the native tests compile
src/services/  own hardware + SD, run FreeRTOS tasks (never touch LVGL)
src/storage/   SD modules: sd_card, atomic_file, attendance_store, photo_store,
               checkin_store, backup_store, sd_tree, battery_log
src/camera/    OV02C10 sensor, csi_pipeline, auto_exposure
src/lcd/  src/touch/  src/rfid/  src/audio/     device drivers
```

- **Only `ui/` includes `lvgl.h`.** Services post `app_event_t` to one FreeRTOS
  queue (`app/event_bus`); `main.cpp` `loop()` drains it on the LVGL thread.
- **SD writes belong on the LVGL thread** — nothing locks the card. A service
  that needs to persist something posts an event and the drain path writes it.
  The named exceptions (photo writer, config/roster retries, the file server's
  own task) and what makes them safe are in ARCHITECTURE.md §Threading.
- Screens: `ui/screen_manager` + `screen_t {create, on_show, on_hide}`.
  `create` runs once/lazily and **before** `s_current` is set (so
  `scr_mgr_current()` is the *previous* screen during create).

## Gotchas that cause real bugs

- **The keyboard textarea UAF is now handled structurally.** LVGL never clears
  `lv_keyboard_t::ta` when its target is deleted, so `lv_keyboard_set_textarea()`
  used to dereference freed memory. `keyboard_make_textarea()` now registers an
  `LV_EVENT_DELETE` handler that releases the keyboard, so deleting a field it
  points at is safe. Screens still call `keyboard_hide()` before an
  `lv_obj_clean()` for the visible behaviour — but forgetting it is no longer a
  crash. **Textareas created directly with `lv_textarea_create()` do not get this
  protection**; use `keyboard_make_textarea()` for anything the shared keyboard
  will serve.
- **Modals with textareas live on the screen root, not `layer_top`**, so the
  shared keyboard floats above them. Overlays parented to the shell root need
  `LV_OBJ_FLAG_IGNORE_LAYOUT` (they're otherwise laid out as flex children).
- **Shared keyboard**: one instance on `layer_top` (`ui/components/keyboard.cpp`),
  reduced numeric map (digits + backspace + OK).
- **On-screen strings are ASCII-only.** Every font in the build covers
  `0x20-0x7F` plus a handful of FontAwesome glyphs, so an em dash, curly quote or
  ellipsis in a label draws as a box. Use `-` and `...`. This applies to anything
  that reaches a label — including a service's `result.message` or an `err`
  buffer, which end up in a toast. `ESP_LOG*` and HTTP responses are exempt (the
  serial console and the browser are not the device's fonts). Accented names from
  a real roster hit the same wall — see BACKLOG.md.
- Watch the LVGL heap: `LV_MEM_SIZE` is 512 KB — allocated from PSRAM, so it
  costs no internal RAM — and `LV_USE_ASSERT_MALLOC=1` turns exhaustion into a
  **silent infinite loop** on the UI thread — a freeze, not a reboot. Prefer
  updating changed widgets over full rebuilds on large lists (roll call).
  Internal RAM is the scarce pool; see ARCHITECTURE.md §Memory budget.
- **Git-tracked, but the user commits manually** — never commit, push, or open
  PRs unless explicitly asked. Deletions are recoverable via git, but still
  confirm before removing files.
- **No RTC** on the board — dates come from `lv_calendar`, not wall-clock time.

## Working style

- Add native tests for new hardware-free logic; add mocks (`lib/hw_mocks/`) for
  new hardware APIs.
- Be direct about hard external deps (esp-dl) and unverifiable hardware paths.
- **Docs: state each fact once.** Test counts live in `test/README.md`, the card
  layout in `docs/software/SD_CARD.md`, the tar schema in
  `docs/software/CONFIG_IMPORT.md`. Link rather than restate — duplicated facts
  are what rotted the previous doc set.
