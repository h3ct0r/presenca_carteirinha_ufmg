# Session handoff — next steps (2026-07-28)

Pick-up notes to continue in a fresh session. Read this + `PROJECT_HANDOFF.md`.

## Where things stand

- **Committed on `main`:** config-import (PRs #1/#2), the class-screen revamp
  (Phases 1–3: hub/session/history nav, per-class ⚙ stats & settings, timed
  double-tap attendance), student avatars on check-in, turma, and the camera
  graceful-bring-up + loopTask stack fixes.
- **Uncommitted (32 files):** the **face-verified kiosk check-in** feature and
  the **class-only photo option** refactor (this is the whole current diff).
- **State:** `pio run -e esp32p4` builds; `pio test -e native` = **118/118**;
  config-builder `node --test` = **72/72**.
- **Git rule:** the user commits/PRs manually — **do not auto-commit or open PRs**
  (see memory `no-auto-commit-or-pr`).

## The uncommitted change (ready to commit)

Face-verified check-in, **kiosk-only**, with photo capture as a **per-class**
option (no device-wide flag). See `docs/software/FACE_CHECKIN.md` (full design +
changelog). Summary:
- New: `storage/checkin_store` (per-student photo path/counter, native-tested),
  `photo_store_encode_to()` (sync encode-to-path), `face_detection_stop()` (safe
  pause), `ui/components/face_verify` (the overlay: preview + box + countdown).
- Kiosk gates each capture-enabled check-in through face-verify; saves
  `/students/checkins/<id>/<date>_<code>_<NN>.jpg`; camera warms on kiosk entry,
  pauses on exit; manual override / non-capture classes unaffected.
- Class-only capture: removed `capture_photos` + `face_verify_seconds` from
  `config.json`/config_service/admin; they are now per-class `class.json` fields
  edited in the class ⚙ settings. Config-builder + all docs updated.

**Suggested commit split:** (1) `feat: per-student check-in photo storage + sync
encoder` (checkin_store, photo_store_encode_to, tests); (2) `feat(kiosk):
face-verified check-in`; (3) `refactor: photo check-in is class-only` (config /
admin / class settings / builder / docs). Or one squashed commit — user's call.

## What's LEFT — prioritized

### 1. Hardware validation — now part of the loop, not a backlog item
**The maintainer manually tests every feature on the real device before starting
the next one**, and as of **2026-07-29 all shipped features work as expected on
hardware.** This is no longer an open batch of unvalidated work; treat it as the
standing workflow: build → `pio test -e native` → flash → exercise by hand on
device → move on.

What an **agent** should still do (it can only run `pio run` / `pio test`):
report its own work as **build- and native-test-verified**, never claim a device
check it didn't perform, and keep calling out the device-only surfaces so the
maintainer knows what to exercise when flashing:
- **Camera/face path** — preview renders, box tracks, countdown ticks, a detected
  face saves a JPEG under `/students/checkins/<id>/`, timeout rejects; camera
  lifecycle across a kiosk session (warm on entry, paused on exit) under
  power/heat/PSRAM and LVGL contention (camera task core 0, LVGL core 1).
- **LVGL rendering** — new screens/overlays, scroll + keyboard behaviour, and
  anything touching the 128 KB LVGL heap.
- **SD/WiFi timing** — import/export, the soft-AP file manager, long writes.

### 2. Student avatar ingestion (BUILT 2026-07-28 — verified on hardware)
The config-builder now ingests the Moodle photo tar (parse → match by name →
re-key to matrícula → re-encode baseline 100×100 → bundle as
`students/photos/<id>.jpg`), and the device importer whitelists + applies that
tree (cap raised 1 MB → 16 MB). New builder modules `untar.js` / `photomatch.js`
(node-tested); firmware `ustar.cpp` / `import_service.cpp` (native-tested,
119/119). See `docs/software/STUDENT_PHOTOS.md` (status + changelog). **Verified
in a browser, against system `tar`, by the native tests, and on the device** —
a real `config.tar` with photos imports and the avatars decode on the check-in /
kiosk / face-verify overlays. Still deferred: streaming unpack, device-side
unknown-id skip+warn, orphan cleanup.

### 3. Check-in photo review aid (Stage 5, deferred)
No way yet to browse `/students/checkins/<id>/` to audit "same person every tap"
— reviewed off-device by opening the folder. An on-device gallery or a
config-builder/desktop viewer is out of scope but wanted eventually.

### 4. On-device config-builder (DESIGNED, not built)
Idea explored 2026-07-28: serve the browser-only config-builder from the ESP32
over the existing soft-AP. Cheap (~23 KB gzipped vs ~2.36 MB free flash; the
device does zero compute — the tool is 100% client-side) and it would give
permanent tool/firmware **schema parity** plus **pre-fill from the live config**.
Full design, measured numbers, security analysis, and a 4-step plan in
`docs/software/ONBOARD_CONFIG_BUILDER.md`. **Decisions taken:** trust WPA2 alone
for now, load passwords as usual, revisit password exposure later. Note it found
two *pre-existing* holes (unauthenticated `/api/upload` to the SD; plaintext
teacher passwords readable via `/api/read`) — both deferred by decision.

### 5. Accented characters don't render (REAL BUG — fonts are ASCII-only)
Every font in the build covers `0x20-0x7F` only (both the stock
`lv_font_montserrat_*` and the custom `font_montserrat_custom_*`). The Latin-1
Supplement (`0xC0-0xFF`) is missing, so **accented Portuguese student names
imported from the Diário CSV render as blanks/boxes on device** — the importer
decodes them correctly, the font just can't draw them. This is read straight off
the fonts' glyph tables (`range_start = 32, range_length = 95`), not inferred:
it likely hasn't been noticed on device yet because the sample rosters use
ASCII-only names ("Maria Santos", "John Miller"). Try a student with an accent to
reproduce. Fix + a ready-to-run `lv_font_conv` command are in
`docs/software/CUSTOM_FONT_GENERATION.md` (see the "Known gap" callout).
Deferred by decision 2026-07-29; the About screen ships ASCII copy until fixed.

### 6. Smaller / optional
- **Config-builder per-class attendance fields:** the builder does NOT emit
  `capture_photos` / `face_verify_seconds` / `timed_attendance` /
  `min_attendance_min`, so importing a `config.tar` **resets** them to defaults
  (documented in CONFIG_IMPORT.md §3.3). If professors author these on a laptop,
  add them to the builder's class editor.
- **Verify state machine test:** the countdown/detect logic is inlined in the
  `face_verify` modal (device-only). Extracting it to a pure, native-tested
  helper was in the plan but skipped as low-value — revisit if it grows.
- **FACE_CHECKIN.md "Staged plan" section** still describes the original
  global-config approach (historical); the Decisions + Changelog are current.

### 7. About screen (BUILT 2026-07-29)
`src/ui/screens/scr_about.cpp` (`SCREEN_ABOUT`): description, authors (Prof.
Hector Azpurua, Aline Molinar — DCC/UFMG), VeRLab + GEAR logos, firmware version,
a scannable QR to the repo (`LV_USE_QRCODE` now `1`), and third-party notices.
Reached from the **Admin → About** card; the idle screen shows only a small
non-interactive `APP_VERSION` string bottom-right (deliberately not a tap target
on the student-facing screen). Logos are RGB565 C arrays in flash
(`src/ui/assets/logo_gear.c`, `logo_verlab.c`, ~26 KB) so they render without an
SD card. Version is `include/app/version.h` (`v0.2-beta`) + a git short SHA
injected by `tools/build/version_flags.py`; it is also the first serial boot line.
**Deliberately NOT in the CSV export** — `MATRICULA,FREQ` is a fixed contract.
Flash 62.5% → 63.0%.

### 8. Multi-professor classes (BUILT 2026-07-29)
A class can be **co-taught**. `class.json` `teacher_email` (scalar) →
**`teacher_emails`** (array, 1..8 = `ROSTER_MAX_CLASS_TEACHERS`, asserted equal to
`CONFIG_MAX_TEACHERS` by the builder's schema-sync test). `class_rec_t` now holds
`teacher_emails[][64]` + `teacher_count`, and `roster_class_matches_teacher()`
matches **any** of them, so the class lists for every professor who teaches it.
**Backward compatible:** a legacy scalar `teacher_email` is still read when the
array is absent, so existing SD cards keep working (native-tested). The
config-builder replaced the teacher dropdown with a **professor checkbox list**
per class, emits the array (de-duplicated), and errors when a class has no
professor selected. Firmware +5.4 KB RAM for the wider struct (12 × 8 × 64 B).
Build-verified + native 124/124 + builder 96/96, and **verified on device.**

## Verify commands
```sh
pio run -e esp32p4                 # device build
pio test -e native                 # host unit tests (124)
cd tools/config-builder && node --test   # web-ui tests (96)
pio device monitor -e esp32p4      # serial (camera/panic logs)
```

## Key pointers
- Feature docs: `FACE_CHECKIN.md`, `CLASS_SCREEN_REVAMP.md`, `CONFIG_IMPORT.md`,
  `STUDENT_PHOTOS.md`, `ONBOARD_CONFIG_BUILDER.md` (design only), and this
  repo's `CLAUDE.md` ("Hardware testing": the maintainer manually validates every
  feature on device; an agent still reports only build/native verification).
- Camera stack: `services/face_detection_service`, `camera/csi_pipeline`,
  `camera/ov02c10_camera`, `storage/photo_store`.
- Check-in flow: `ui/screens/scr_kiosk.cpp` (`check_in` → `face_verify_open`),
  `ui/components/face_verify.cpp`, `storage/checkin_store.cpp`.
