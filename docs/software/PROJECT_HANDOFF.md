# Project Handoff — RFID Attendance Device (ESP32-P4 + LVGL)

**Deep reference + living project state.** This is the detailed record: full
architecture, per-screen behavior, the SD data model, the session log, and the
**code-review backlog with statuses** (below). It captures the current
in-progress work. Last major session: **2026-07-23**.

> **Two-file setup:** the root **`CLAUDE.md`** auto-loads every session and holds
> the stable *rules* (build/test commands, layer boundaries, the bug-causing
> gotchas). **This file** holds the volatile depth — the backlog, "START HERE"
> state, and reference material you don't want re-read into context every turn.
> No need to paste it into a new chat; open it when you need the detail or are
> updating project state. When rules change, update CLAUDE.md; when work
> progresses, update this file.

> **Hardware testing (important):** the maintainer **manually tests every
> feature on the real device** before starting the next one — each change is
> flashed and exercised by hand as part of the development loop, and as of
> 2026-07-29 all shipped features behave as expected. An agent, however, can only run
> `pio run` / `pio test`: report your own work as **build- and
> native-test-verified**, never claim a device check you didn't perform, and keep
> flagging the device-only surfaces (LVGL, camera, WiFi, SD timing, stack). See
> the same section in `CLAUDE.md`.

---

## ▶ START HERE — current state

### Current state: all shipped features verified on device (2026-07-29)
Each feature is manually exercised on hardware before the next one begins, and
everything shipped so far behaves as expected — including the camera + ESP-DL
face detection path, which was the long-running blocker below.

**Resolved: face detection detects 0 faces (was "case 3").** The camera pipeline
initialized and the model loaded from SD (`Detector: LOADED`), but inference
returned **0 faces**. The fix was restoring the original **3-way pixel-format
auto-discovery** (RGB565-LE / RGB565-BE / manual RGB565→RGB888) in
`run_detection()` — the 888 path had been dropped in a refactor. The camera
status line shows the active format + luma: `frame N  faces K [fmt]  infer Xms
luma L`.

Kept for future debugging, if detection ever regresses — open camera, stand in
front, read that status line:
- `faces ≥1 [888]` (or LE/BE) → the working case; that format is being used.
- `faces 0 [none]` → not a format issue. Check whether the preview image looks
  correct, and what `luma` reads (healthy ≈ 60–160; ~0 or ~255 = broken
  exposure/pipeline) — those point at CSI/ISP/AE rather than detection.
- Image fine but still `[none]` → look at model input size/normalization.

See `docs/FACE_DETECTION.md`. Models must be at `/models/*.espdl` on the SD card
(staged in `docs/software/sd_card_example/models/`); a missing model no longer crashes
(guarded), it runs preview-only.

### Deferred backlog (from the 2026-07-23 code review)
A full assessment was done; **C1, C2 and S2 are complete**, the rest are open.
See the "Code-review findings" section below. Highest value remaining: **C3**
(file server blocks LVGL thread), the **D-group dead-code cleanup** (note:
`photo_store` is no longer dead — it's the JPEG snapshot writer now), the **modal
component extraction (Q2)**, and small UX items (**U1/U2/U3**).

### Offline config import — builder half M1+M2 built; device half not started
A two-part feature to author config on a laptop and import it without losing
attendance. The **contract** both sides follow is
[`docs/software/CONFIG_IMPORT.md`](CONFIG_IMPORT.md) (authoritative tar format + schemas,
mirrored from the roster/config validators — now written, v1).

1. **`tools/config-builder/`** — off-device, browser-only tool that generates a
   `config.tar`. **Status: M1–M4 core done** (2026-07-25). Vanilla JS, no
   build/CDN: DOM-free `src/{uid,tarball,model,validate,diario}.js` (unit-tested,
   **49 `node --test` cases green**) + a working `index.html`/`app.js` authoring
   page (teachers/students/classes editors, live validation gating export, CSV
   paste, save/load model JSON). Tar verified via system `tar` and byte-identical
   in-browser vs Node. Validation mirrors the firmware and is intentionally
   stricter where the contract demands (length caps, student↔teacher UID
   collision, teacher_email resolution) — marked `[builder-stricter]` in source.
   **Diário de Classe CSV import** (`diario.js`): reads the UFMG semicolon export
   (matricula+nome only), handles accents/Latin-1, and auto-creates the class
   keyed `<SEMESTER>-<ATIVIDADE>` (e.g. `2026_2-DCC219`; semester `/` → `_`, so
   all turmas of a course+semester merge into one class) + tags each roster entry
   with `turma`. Remaining: minor
   UX polish (focus retention on re-render, inline field highlighting). **Not yet
   round-tripped through real firmware.**

   **`turma` schema field** (2026-07-25): a **per-student tag on each
   `class.json` roster entry** (`{ "id", "turma" }`, ≤15) — **not** on the global
   student registry, **not** class-level, so one class can span turmas and a
   student differs per class. Updated end-to-end: `CONFIG_IMPORT.md` §3.3 +
   changelog, `sd_card_example`, the tool (`validate.js` `ROSTER_TURMA`,
   `diario.js`, per-roster UI), and a native test proving the loader accepts
   `{id,turma}` entries. Additive/compatible — the firmware reads only `id` and
   preserves `turma` on rewrite (DOM edit in `persist_enroll`), so **no
   `roster.h` struct change** and file `version` stays 1.
2. **Device import endpoint** (firmware, `src/services/…`) — stage → validate →
   merge the tar, preserving attendance/photos. **Not started.** See
   `CONFIG_IMPORT.md` §5 for the intended flow (back up first, sandbox unpack
   with the §4 path whitelist, per-file atomic apply). This is the natural next
   chunk; it can reuse the roster/config validators and `backup_store_create()`.

The config-builder lives **in this repo** on purpose (shared schema = one source
of truth; no git to coordinate two repos; PlatformIO ignores `tools/`). It uses
**no C++/LVGL rules** — see its directory-scoped `CLAUDE.md`.

### Planned: Moodle student photos → device (design done, not built)
Bundle student photos (downloaded from Moodle as a tar of `NAME.jpg` files) into
the same `config.tar`, re-keyed by matrícula to `students/photos/<id>.jpg`, and
show a student's photo on the presence-success screen. Full design + phased plan
(browser-side name→id matching, tar contract whitelist/cap changes, firmware
import + JPEG-on-SD display) is in [`STUDENT_PHOTOS.md`](STUDENT_PHOTOS.md).
**Idealisation only — no code yet.**

---

## Session log — 2026-07-23 (+ late 2026-07-22)

All build + native-test verified unless noted; **not run on hardware**.

**Attendance / export**
- **CSV Export** (`SCREEN_EXPORT`, `scr_export.cpp`, `services/export_service.cpp`,
  `docs/EXPORT.md`). Multi-select: each class is a **big checkbox**; a single
  **"Export Selected (N)"** button (disabled when none ticked); **"Select all"**
  shortcut; one **overwrite-all** confirm if any target exists (not per file).
  Writes `/csv_export/<code>.csv` = `MATRICULA,FREQ` (**FREQ = absent session
  days per student**). Snapshots/restores the open attendance session. 6 tests.
- **History tab** shows attendance **% per row**; roll call **sorted by name**.

**Admin panel (renamed from Account)**
- `scr_account.{cpp,h}` → **`scr_admin.{cpp,h}`**, `SCREEN_ACCOUNT` →
  `SCREEN_ADMIN`, `nav_account_cb` → `nav_admin_cb`. Class list removed;
  sections ordered **profile → SD card → settings → password → debug → Sign
  Out** (settings/camera-preview sits above password; 16px gap between them);
  Sign Out built once (not rebuilt each show).
- **SD-card usage meter** (`sd_card_usage()` in `sd_card.cpp`; mock got
  `totalBytes/usedBytes`). Progress bar + "X of Y used (Z%)".
- **Password editor**: passwords are now **digits-only** (new status
  `CONFIG_NON_NUMERIC_PASSWORD`). Write API `config_set_password(email,uid,pw)`
  → `config_result_t` (atomic rewrite + reload). "Set/Change password" modal
  with two numeric confirm fields.
- **Debug section**: a toggle reveals a red **"Delete all cards & attendance"**
  button → confirm modal → `roster_clear_all_uids()` (clears every rfid_uid,
  rewrites students.json) + `attendance_clear(dir)` for every class. 2 tests.
- **Camera section**: a **"Camera preview"** button → `SCREEN_CAMERA`. Photo
  check-in is configured **per class** (in the class ⚙ settings), not device-wide.

**config_service**
- No capture flag in config.json — photo check-in is a per-class option
  (`class.json` `capture_photos` + `face_verify_seconds`, see roster_service).
- **C2**: lightweight lookups `config_find_teacher_by_uid/by_password`,
  `config_teacher_has_password` — iterate under the lock, copy out only the
  matched teacher, so callers don't put a ~1.5 KB `device_config_t` on the LVGL
  stack. `auth.cpp` is now a thin delegate to these.

**WiFi File Editor (debug)** — `SCREEN_WIFI_EDITOR`, `scr_wifi_editor.cpp`
- `services/wifi_ap.cpp`: soft-AP on the ESP32-C6 (`WiFi.softAP`), SSID
  `CARTEIRINHA-XXXX` + 8-digit numeric password (per boot). **Restart fix**:
  stop with `softAPdisconnect(false)` (keep the stack/C6 link alive) — fully
  powering off broke the next start.
- `services/file_server.cpp`: a synchronous `WebServer` on :80 **pumped from an
  LVGL timer** (all SD I/O on the LVGL thread). Single-page file manager
  (offline Tailwind-style CSS embedded — no CDN): list/navigate, download,
  **upload**, **edit-in-browser**, delete; hidden-dotfiles toggle; folder icon;
  Esc closes the editor. **Confirmed on hardware: AP works, page loads.**
- Screen: shows SSID/password in clear text, Start/Stop, green top-bar WiFi icon
  + green button while live, big centered IP. Footer nav is **disabled while the
  AP is up** (can't leave it running). "Turning on WiFi…" shown via `lv_refr_now`
  before the blocking `softAP` call.
- ⚠ **Security**: no auth, plain HTTP, full read/write incl. `config.json`
  passwords. Debug-only. (Assessment S1.)

**Camera + face detection** — see dedicated section below.

**UI polish / fixes**
- Battery **voltage on the top bar** beside %. Battery curve is now **4.0 V=100%
  / 2.7 V=0%, 65 mV anchors** (user changed it from 4.2/3.2; tests match).
- Idle login: numeric keypad, **auto-opens**, **OK-unlocks**, reduced digits+OK
  keymap (no +/-, decimal, keyboard-switch); "Enter password" is a white button.
  Enroll-unlock modal also numeric + auto-open.
- **Footer active-tab highlight** (`shell_set_active_nav`, accent fill).
- **Modal fullscreen fix**: overlays parented to the shell root were laid out as
  flex children (misrendered). Added `LV_OBJ_FLAG_IGNORE_LAYOUT` to the export,
  admin-password, class-unlock, and kiosk-exit modals.
- **Class list** shows the class **code**; header Enroll/Kiosk word labels
  (`shell_add_action` optional label); session callout restyled (amber notice).
- **Kiosk exit gate**: professor card/password required to leave kiosk.

**Crash fixes (hardware-reported)**
- **Stack overflow saving password**: `config_set_password` used a 2 KB stack
  buffer *and* called `load_config` (another 2 KB) → `loopTask` overflow. Fixed
  with a heap read buffer freed before `load_config`.
- **Use-after-free enrolling then Session tab**: `enroll_goto()` cleaned the
  enroll widgets (textareas the shared keyboard pointed at) **without hiding the
  keyboard first** → the next `keyboard_hide()` touched freed memory. Fixed by
  calling `keyboard_hide()` before `lv_obj_clean()` in `enroll_goto` (mirrors
  `rebuild_content`). **Every `lv_obj_clean` that deletes keyboard textareas must
  `keyboard_hide()` first.**
- **C1 (stability)**: `LV_MEM_SIZE` 64 KB → **128 KB** (`lv_conf.h`) — a full
  class roll call could approach 64 KB and `LV_USE_ASSERT_MALLOC=1` turns
  exhaustion into a reboot.

**Native test count: 78** (9 suites; +export +photo-capture +clear-uids +
clear-attendance +teacher-has-password, and +backup (4 cases) this session).

---

## Code-review findings (2026-07-23) — status

Full assessment done at the user's request. Severity + status:

- **C1 LVGL heap** — ✅ done (LV_MEM_SIZE 128 KB). Also *should* stop full-
  rebuilding the roll call (updates only the changed chip) — not done.
- **C2 config stack copies** — ✅ done (lightweight lookups).
- **C3 file server blocks LVGL thread** — open. Large transfers freeze the UI
  (it's pumped on the LVGL timer). Acceptable for debug; move to a task + SD
  mutex if it bites.
- **S1 file server no auth / cleartext / no path-traversal guard** — open.
- **S2 destructive wipe has no backup/undo** — ✅ done (backup-only). The debug
  wipe now calls `backup_store_create()` (`storage/backup_store.cpp`) first,
  snapshotting `students.json` + every class's attendance logs into `/backup/`
  (mirrored paths, single slot cleared each time). The wipe **aborts if the
  backup fails**. Restore is manual via the WiFi file editor (no in-app restore).
  4 native tests.
- **D1 dead screens** — `scr_teacher_reg` was **removed 2026-07-29** (it was an
  unreachable stub whose "Waiting for card..." box never captured a card and
  whose Confirm discarded everything — dangerous to leave around looking
  functional). Still open: `scr_student` and `scr_class_form` are registered but
  **never navigated to** (0 inbound); `scr_class_form` is likewise a
  non-functional demo stub. Deletions are recoverable via git, but confirm
  before removing.
- **D2 `photo_store`** — RESOLVED: it's now wired as the JPEG snapshot writer.
- **D3** — leftover commented test block in `scr_idle.cpp:~383`.
- **U1** footer "WiFi File Editor" label wraps (longest of 4 tabs).
- **U2** kiosk-exit password (`s_exit_ta`) still alphabetic (others are numeric).
- **U3** export "Export Selected" button scrolls with the list (pin it).
- **U4** CSV `MATRICULA` is the id verbatim (may contain dashes).
- **Q1** `scr_class.cpp` is an 890-line god-screen (split enroll out).
- **Q2** the modal overlay pattern is copy-pasted ~6× — extract a
  `ui/components/modal` / `ui_confirm(...)` (would also centralize the layering
  rules that caused the IGNORE_LAYOUT bug). **High value.**
- **Q3** stale `main.cpp` log ("face detection starting") / comments.
- **Q4** stale `battery_curve.cpp` comment ("50 mV" → now 65 mV); 2.7 V=0% is a
  deep cutoff, and battery ADC/divider are unverified on hardware.
- **F items** — per-day timestamps (needs RTC/NTP), backup/restore, on-device
  class/teacher management (finish or delete the stubs), kiosk auto-timeout.

---

## What this is

A classroom attendance device: students register presence by tapping an RFID
card (or, in kiosk mode, typing their university ID). A touchscreen (LVGL 9)
runs the UI; an SD card holds all data (config, students, classes, attendance,
CSV exports, face-detect models). WiFi (via a companion chip) drives a debug
file editor. An onboard camera + ESP-DL do (in progress) face detection.

**Workflow:** professor taps their card at idle → picks a class → opens a dated
session → takes roll call (tap cards/names) or enrolls students (auto-checked
in) or starts an unattended kiosk for self-check-in. Attendance exports to CSV;
config/data are editable over the WiFi file server.

## Hardware (board: Guition JC4880P443C, module JC-ESP32P4-M3)

Schematics/datasheets in `docs/`.
- **MCU:** ESP32-P4. No native WiFi/BT — WiFi on an external **ESP32-C6**
  companion (via esp_wifi_remote; the Arduino P4 core bundles it). **ADC2 free.**
- **Display:** ST7701, 480×800 portrait, RGB565, MIPI-DSI. Two PSRAM
  framebuffers. `src/lcd/`, `src/lvgl_port.cpp`.
- **Touch:** GT911 over Wire1 (I2C port 1, GPIO7 SDA / GPIO8 SCL). `TP_INT`
  not wired; polled. `src/touch/`.
- **Camera:** OV02C10 2MP MIPI-CSI (I2C addr 0x36 on port 1, self-clocked, no
  reset/pwdn). `src/camera/` (sensor + CSI pipeline + software AE/AWB).
- **No RTC** → dates chosen with `lv_calendar` (placeholder = last session date).
- **Power:** IP5306 + TLV62569. **SW3 is the power button** — on battery it
  MUST be pressed to boot. Light-load auto-shutdown (~<50 mA/32 s).
- **Battery:** BAT+ →68k→ **GPIO53 (ADC2_CH4)** →100k→ GND, `V_BAT = V_node ×
  1.68`, `analogReadMilliVolts(53)` @12dB. `services/battery_service.cpp`.
- **SD:** SD_MMC on IOMUX pins, `SD_MMC.begin("/sdcard")` — no pin setup, VFS
  mount at `/sdcard`. Centralized in `storage/sd_card.cpp` (`sd_card_mount()`).
- **Audio:** ES8311 → NS4150 → speaker. I2S GPIO 13/12/10/9, PA_CTRL=GPIO11.
  `src/audio/beeper.cpp`.
- **RFID:** PN532 on Wire1 (Adafruit lib), polled. `src/rfid/pn532_reader.cpp`.

## Build & test

```sh
pio run -e esp32p4          # build device firmware
pio test -e native          # host unit tests (78 cases, 9 suites)
pio test -e native -f "native/test_roster"   # one suite
```

`platformio.ini`: `esp32p4` (device, compiles all of `src/`) and `native`
(host tests via `build_src_filter`, hardware-free sources against
`lib/hw_mocks/`). **Add every new hardware-free `.cpp` to the native filter.**
`lib_ignore = hw_mocks`. The esp32p4 env now also has extensive **esp-dl build
wiring** (include paths, `CONFIG_*` defines, `-lfbs_model`, `-D USE_FACE_DETECT`,
SD model config) — see the face-detection section.

## Architecture (strict layers, events flow up)

```
ui/        LVGL screens/components/theme — ONLY code that includes lvgl.h
app/       pure logic: event_bus, auth, session, uid, roster/teacher types, battery_curve
services/  own hardware + SD, run FreeRTOS tasks: config, roster, rfid, battery,
           export, wifi_ap, file_server, face_detection
storage/   SD modules: sd_card (mount), attendance_store, photo_store (JPEG
           writer), backup_store (pre-wipe snapshot to /backup)
camera/    OV02C10 sensor, csi_pipeline, auto_exposure (driver layer)
drivers/   lcd/, touch/, rfid/, audio/
```

- **Rule:** only `ui/` includes lvgl.h; services never touch LVGL. Services post
  `app_event_t` to one FreeRTOS queue (`app/event_bus`); `main.cpp` `loop()`
  drains it on the LVGL thread → `ui_handle_event()` → `lv_timer_handler()`.
- **Screens:** `ui/screen_manager` + `screen_t {create, on_show, on_hide}`.
  `create` lazy/once; per-visit state in `on_show`. IDs in `include/ui/screen.h`.
  **Gotcha:** `create()` runs *before* `s_current` is set, so `scr_mgr_current()`
  is the *previous* screen during create.
- **Reactive state:** `ui/ui_state.cpp` `lv_subject` observables (wifi rssi,
  wifi-AP active, battery %, battery mv, config/roster status, user name).
- **Card capture:** `ui_set_card_capture(cb)` diverts the next scanned card to a
  flow (one-shot; re-arm as needed; cleared on hide).

## Screens (src/ui/screens/)

- `scr_idle` — dark access gate; professor card or numeric password → session →
  Classes. Shared password modal (numeric, auto-opens, OK unlocks).
- `scr_classes` — professor's class list (shows class **code**).
- `scr_class` — **the big one (890 lines, Q1)**: Session / History / Enroll tabs,
  lock + kiosk header actions. See below.
- `scr_admin` — profile, SD usage, settings (photos + camera preview), password,
  debug (wipe), Sign Out. Reached via footer "Admin".
- `scr_export` — CSV export (checkbox multi-select). Footer "CSV Export".
- `scr_wifi_editor` — debug WiFi AP + file server. Footer "WiFi File Editor".
- `scr_camera` — camera preview + face boxes + model diagnostics. From Admin.
- `scr_kiosk` — unattended self check-in (exit is professor-gated).
- **Dead (D1):** `scr_class_form` (demo stub, 0 inbound nav), `scr_student`
  (superseded confirmation screen). `scr_teacher_reg` was removed 2026-07-29.

Footer nav (`shell.cpp add_footer`, on Classes/Export/WiFi/Admin): Classes /
Export / WiFi File Editor / Admin. Active tab highlighted; nav can be disabled
(WiFi screen while AP up).

## SD card data model (see docs/software/sd_card_example/)

```
/config.json      { "teachers":[{name,email,rfid_uid,password}] }
/students/students.json   { version, students:[{id,name,rfid_uid|null}] }
/students/photos/<id>.jpg (reference avatar)   /students/checkins/<id>/*.jpg (face-verify)
/classes/<CODE>/class.json  { version,code,name,schedule,teacher_emails[],color,
                              capture_photos?,face_verify_seconds?,timed_attendance?,min_attendance_min?,
                              roster:[{id,turma?}] }
/classes/<CODE>/attendance/YYYY-MM-DD.jsonl   append-only {"id":..,"present":bool}
/csv_export/<CODE>.csv     MATRICULA,FREQ export (see docs/EXPORT.md)
/models/*.espdl            ESP-DL face-detection models (see docs/FACE_DETECTION.md)
/backup/students/students.json + /backup/classes/<CODE>/attendance/*.jsonl
                           pre-wipe snapshot (single slot; backup_store)
```

- **config.json** (`config_service`): per-professor **unique digits-only**
  passwords (`CONFIG_DUP_PASSWORD` / `CONFIG_NON_NUMERIC_PASSWORD`). Photo
  check-in is per-class (class.json), not a device flag. Lightweight lookups
  avoid whole-struct copies.
- **Registry model:** students are a global registry keyed by university id;
  classes reference by index; one record + one card binding per student.
- **roster_service:** loads + strictly validates; writes are atomic
  (measureJsonPretty→heap buf→temp→remove+rename). `roster_enroll_existing/new`,
  `roster_clear_all_uids` (debug wipe). Rejects a uid owned by another student or
  a professor.
- **attendance_store:** one JSONL/day, fold = last-write-wins. `attendance_open/
  close/set/is_present/present_count/list_dates/present_for/clear`.

## Class screen (scr_class.cpp) — key behaviors

- **Session tab:** open session → roll call (green chips, sorted by name; tap
  name or scan card to mark; "Close session"); else → `lv_calendar` picker +
  "Open session" + recent dates.
- **History tab:** past dates "N/total present" **+ %**, tap to edit.
- **Enroll tab:** gated behind professor unlock AND an open session. Search class
  roster / add new student → wait for card → binds (students.json + class.json) →
  auto-checks in. `enroll_goto()` **hides the keyboard before cleaning** (UAF fix).
- **Header actions:** lock/unlock (gates Enroll; numeric-password modal),
  kiosk (enabled while session open).
- **Tab restyle gotcha:** `rebuild_content` uses `lv_obj_remove_style(btn,&style,
  0)` not `remove_style_all`.

## Kiosk screen (scr_kiosk.cpp)

Self check-in by RFID or typed ID (digit-only match). Dedicated numeric keypad.
Full-screen green/red result overlay. **Exit is professor-gated** (card or
password modal). Exit password field is still alphabetic (U2).

## Camera + face detection (in progress)

Docs: `docs/FACE_DETECTION.md`. Memory: `camera-face-detection.md`.
- **Driver layer** `src/camera/`: `ov02c10_camera` (sensor), `csi_pipeline`
  (CSI→ISP→RGB565 `get_frame`), `auto_exposure` (software AE/AWB). Kept as-is.
- **Service** `services/face_detection_service.{h,cpp}` (headless, no LVGL): owns
  pipeline + PPA downscale 1920×1080→480×270 + detection task (core 0). API:
  `face_detection_start/running/snapshot/status/model_info/request_capture`.
  Face inference behind `#ifdef USE_FACE_DETECT`. **Guards against missing
  models** (esp-dl faults on load failure) → preview-only if absent.
- **Screen** `scr_camera` (`SCREEN_CAMERA`): standard **`shell` chrome**
  (gradient header + back button + "Camera / Face detection preview" title, like
  the class-detail screen), then a **full-bleed 480×270 preview** (`lv_image` +
  red box overlays; body padding zeroed so face-box coords stay 1:1 at native
  size), and a padded control column with a **status card**, a full-width
  **"Take picture"** button, and the **model diagnostic card** (paths, sizes,
  LOADED?). Started/stopped via on_show/on_hide (camera stays on once started —
  always-on; pause = follow-up). **Snapshot popup**: the refresh timer polls
  `photo_store_last_saved()` (returns the full path + a save counter); when the
  counter bumps it shows a centered "Picture saved" modal with the SD path
  (`IGNORE_LAYOUT` overlay on the shell root, OK to dismiss). `on_show` syncs the
  counter first so an earlier photo doesn't re-pop.
- **ESP-DL**: vendored `lib/esp-dl` (P4 subset) + `lib/human_face_detect`.
  `platformio.ini` wires include paths, `CONFIG_*`, `-lfbs_model`, `-D
  USE_FACE_DETECT`. **Critical**: `lib/human_face_detect/library.json` declares
  `dependencies:{esp-dl}` — without it the LDF finds headers via `-I` but never
  compiles esp-dl's sources → `undefined reference to dl::...` at link.
- **Models load from SD** (`CONFIG_HUMAN_FACE_DETECT_MODEL_IN_SDCARD`,
  `SDCARD_DIR="models"`): `/models/human_face_detect_{msr,mnp}_s8_v1.espdl`. No
  packing/flashing (the `human_face_det` flash partition is unused now). Staged
  in `docs/software/sd_card_example/models/`.
- **photo_store** (`storage/photo_store.cpp`): background writer; **JPEG via P4
  hardware encoder** (quality 85, ~300–500 KB @1080p) → `/photos/IMG_nnnn.jpg`;
  BMP only if the encoder is unavailable.
- **Status: working on hardware** (see "START HERE" — the 0-faces issue was a
  missing RGB888 pixel-format path and is resolved). Per-student face-verify
  check-in is wired (kiosk-only, per-class `capture_photos`; saves
  `/students/checkins/<id>/*.jpg`) and verified on device — see
  docs/software/FACE_CHECKIN.md.

## Theme / fonts / toast

- `ui/theme/theme.cpp`: light "staff" + dark palette; `ui_make_button/label/card`,
  `ui_add_press_feedback` (opacity-dim + `beeper_touch`); `UI_BUTTON_HEIGHT=72`.
  `THEME_WARNING`/`THEME_WARNING_SOFT` (amber) added.
- Beeps: `beeper_touch` (tick), `beeper_beep` (grant), `beeper_error` (two-tone).
- Custom fonts `src/ui/assets/font_montserrat_custom_{14,20,32}.c` (FontAwesome
  glyphs: lock/unlock/users-viewfinder/id-card).
- `ui/components/toast.cpp`: `ui_toast_show(msg, ok)` on layer_top.
- **Shared keyboard** `ui/components/keyboard.cpp`: one keyboard on layer_top;
  reduced numeric map (digits+backspace+OK); `keyboard_show`, `keyboard_set_
  ready_cb` (OK action), `keyboard_hide` (clears the ready cb). **Modals with
  textareas must live on the screen root, not layer_top, so the keyboard floats
  above them.**

## Testing (test/native/, 78 cases, 9 suites)

Unity + `lib/hw_mocks/` (in-memory SD with FILE_APPEND/remove/rename/mkdir/
totalBytes/usedBytes, fake PN532, fake JPEG, pthread FreeRTOS). Suites: core
(uid/session/battery curve), event_bus, rfid, auth_config (+password write,
+photo-capture, +teacher-has-password), roster (+enroll, +clear_uids), export,
attendance (+clear), photo, backup (pre-wipe snapshot). **Gotchas:**
`sd_card_mount()` latches per binary (no-SD tests first); some tests chain shared
config state (order matters). UI/LVGL, camera, WiFi, esp-dl are NOT native-tested.

## Working style established with the user

- After each change: `pio run -e esp32p4` AND `pio test -e native`; report
  honestly as "build- and native-test-verified", and say plainly that you have
  not run it on hardware. The user then flashes and manually exercises the
  feature on the device before the next one starts — that step is theirs, never
  claim it as done.
- Add native tests for new hardware-free logic; add mocks for new HW APIs.
- Be direct about hard external deps (esp-dl) and unverifiable hardware paths.
- Project memory lives in the Claude Code memory dir (MEMORY.md +
  power-architecture / config-file / sd-data-layout / testing-setup /
  camera-face-detection). Auto-loads in a new session in this repo.
