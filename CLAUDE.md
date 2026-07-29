# CLAUDE.md — RFID Attendance Device (ESP32-P4 + LVGL)

Classroom attendance device: students tap an RFID card (or type their ID in
kiosk mode) to register presence. LVGL 9 touchscreen UI; all data on an SD card.
See `docs/software/PROJECT_HANDOFF.md` for the full architecture, session log, and the
open code-review backlog — this file is the always-loaded short version.

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

## Hardware testing (important)

**The maintainer manually tests every feature on the real device before starting
the next one.** Each change is flashed and exercised by hand on hardware as part
of the development loop, so shipped features here are device-verified, not just
build-verified. As of 2026-07-29 all of them behave as expected on device.

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
ui/        LVGL screens/components/theme — the ONLY code that includes lvgl.h
app/       pure logic: event_bus, auth, session, uid, roster/teacher types
services/  own hardware + SD, run FreeRTOS tasks (never touch LVGL)
storage/   SD modules: sd_card, attendance_store, photo_store
camera/    OV02C10 sensor, csi_pipeline, auto_exposure
drivers/   lcd/, touch/, rfid/, audio/
```

- **Only `ui/` includes `lvgl.h`.** Services post `app_event_t` to one FreeRTOS
  queue (`app/event_bus`); `main.cpp` `loop()` drains it on the LVGL thread.
- Screens: `ui/screen_manager` + `screen_t {create, on_show, on_hide}`.
  `create` runs once/lazily and **before** `s_current` is set (so
  `scr_mgr_current()` is the *previous* screen during create).

## Gotchas that cause real bugs

- **Every `lv_obj_clean()` that deletes keyboard textareas must call
  `keyboard_hide()` FIRST** — otherwise the next `keyboard_hide()` touches freed
  memory (a fixed UAF). See `enroll_goto` / `rebuild_content`.
- **Modals with textareas live on the screen root, not `layer_top`**, so the
  shared keyboard floats above them. Overlays parented to the shell root need
  `LV_OBJ_FLAG_IGNORE_LAYOUT` (they're otherwise laid out as flex children).
- **Shared keyboard**: one instance on `layer_top` (`ui/components/keyboard.cpp`),
  reduced numeric map (digits + backspace + OK).
- Watch the LVGL heap: `LV_MEM_SIZE` is 128 KB and `LV_USE_ASSERT_MALLOC=1`
  turns exhaustion into a reboot. Prefer updating changed widgets over full
  rebuilds on large lists (roll call).
- **Git-tracked, but the user commits manually** — never commit, push, or open
  PRs unless explicitly asked. Deletions are recoverable via git, but still
  confirm before removing files (e.g. the dead `scr_*` stubs).
- **No RTC** on the board — dates come from `lv_calendar`, not wall-clock time.

## Working style

- Add native tests for new hardware-free logic; add mocks (`lib/hw_mocks/`) for
  new hardware APIs.
- Be direct about hard external deps (esp-dl) and unverifiable hardware paths.
