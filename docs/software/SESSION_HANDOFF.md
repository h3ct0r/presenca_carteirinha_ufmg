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

### 1. Hardware validation (the big one — nothing below the API layer has run)
Everything camera/LVGL is **build-verified only**. On device, verify:
- **Face detection actually detects faces.** `PROJECT_HANDOFF.md` "START HERE"
  notes the camera inits + model loads but **0 faces detected** on hardware. If
  that's still true, **face-verify will always time out** — this is the #1
  blocker for the whole feature. Debug `face_detection_service` /
  `human_face_detect` (model format, pixel format tried, thresholds) first.
- **Camera lifecycle under kiosk.** The camera stays warm through a kiosk session
  (`face_detection_start` on entry, `face_detection_stop` pause on exit). Watch
  for power/heat/PSRAM issues or LVGL contention (camera task core 0, LVGL core
  1). Fallback if unstable: start-per-tap (FACE_CHECKIN.md option B).
- **The verify overlay** (`face_verify`): preview renders, green box tracks the
  face, countdown ticks, a detected face saves a JPEG under
  `/students/checkins/<id>/` and registers the tap; timeout rejects.
- **Class-only UI:** admin "Camera" card (preview only), class ⚙ **Photo
  check-in** switch + **Face-verify time** field save/persist.
- Also re-check the earlier build-verified UI on device: class hub/session nav,
  timed attendance, kiosk numeric exit keypad, class-settings scroll/keyboard.

### 2. Student avatar ingestion (BUILT 2026-07-28 — validate on hardware)
The config-builder now ingests the Moodle photo tar (parse → match by name →
re-key to matrícula → re-encode baseline 100×100 → bundle as
`students/photos/<id>.jpg`), and the device importer whitelists + applies that
tree (cap raised 1 MB → 16 MB). New builder modules `untar.js` / `photomatch.js`
(node-tested); firmware `ustar.cpp` / `import_service.cpp` (native-tested,
119/119). See `docs/software/STUDENT_PHOTOS.md` (status + changelog). **Verified
in a browser + against system `tar` + native tests; NOT run on device** — flash
a real `config.tar` with photos and confirm avatars decode on the check-in /
kiosk / face-verify overlays (JPEG-on-SD via TJPGD is the untested path). Still
deferred: streaming unpack, device-side unknown-id skip+warn, orphan cleanup.

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

### 5. Smaller / optional
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

## Verify commands
```sh
pio run -e esp32p4                 # device build
pio test -e native                 # host unit tests (119)
cd tools/config-builder && node --test   # web-ui tests (89)
pio device monitor -e esp32p4      # serial (camera/panic logs)
```

## Key pointers
- Feature docs: `FACE_CHECKIN.md`, `CLASS_SCREEN_REVAMP.md`, `CONFIG_IMPORT.md`,
  `STUDENT_PHOTOS.md`, `ONBOARD_CONFIG_BUILDER.md` (design only), and this
  repo's `CLAUDE.md` (honesty rule: report as
  build/native-verified, **not run on hardware**, unless actually flashed).
- Camera stack: `services/face_detection_service`, `camera/csi_pipeline`,
  `camera/ov02c10_camera`, `storage/photo_store`.
- Check-in flow: `ui/screens/scr_kiosk.cpp` (`check_in` → `face_verify_open`),
  `ui/components/face_verify.cpp`, `storage/checkin_store.cpp`.
