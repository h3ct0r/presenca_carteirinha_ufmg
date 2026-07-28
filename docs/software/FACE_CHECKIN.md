# Face-verified check-in + per-student check-in photos

When a class enables photo check-in (a per-class option; there is no device-wide
flag), a kiosk check-in is only accepted after a **face is detected on camera
within a countdown window**, and the captured frame is saved on the SD card
**grouped by student** so a reviewer can later flip through a student's check-in
photos and confirm the same person tapped every time.

Status: **Stages 1–4 implemented** (Stage 5 = docs, this file). Storage/config
are native-tested; the camera lifecycle + verify modal are build-verified only
and **must be validated on hardware** (the camera is the surface that crashed
before). See the changelog at the end.

## Goal (from the request)

1. On an RFID tap or typed university ID, if the class's photo option is active
   (a per-class option — there is no device-wide flag), the student has **N
   seconds** (default 15, configurable per class) to show their face.
2. The tap is valid **only if a face is detected** within that window.
3. The photo is saved so it is simple, later, to verify the **same person**
   tapped for the **same student** over time.
4. The verification UI shows: a "show your face" prompt, a **countdown**, and a
   **face-detected indicator** (the bounding box).

## Decisions

- **Kiosk-only.** Face-verified capture runs **only in kiosk mode**. A tap on the
  main class Session roll call registers attendance directly (no camera). Rationale:
  kiosk is the unattended self-check-in flow where identity verification matters;
  the roll call is professor-driven.
- **Activation** is the per-class `class.json` `capture_photos` bool
  (`class_capture_enabled(cls)`). **Class-only — there is no device-wide capture
  flag.** Evaluated only in kiosk.
- **Timeout**: per-class `face_verify_seconds` in `class.json` (default 15,
  clamped 3–60), edited in the class ⚙ settings. The Admin screen keeps only the
  camera preview (no capture toggle, no global timeout).
- **Gate semantics**: a capture-enabled tap registers attendance **only** after a
  face is seen. Timeout with no face ⇒ the tap is rejected ("no face detected —
  tap again"). This is the key behavior change.
- **A photo is saved per accepted tap** (each check-in event), not once per
  session — that is what lets you audit "same person, every tap."
- **Timed mode (Phase 3) interaction**: face-verify gates each accepted tap, so
  in timed mode both the tap-in and tap-out are verified (a photo each). The
  verify modal is the same either way.

## Storage layout

A new tree, keyed by student id, distinct from the reference avatar
(`/students/photos/<id>.jpg`) and the camera's raw snapshots (`/photos/**`):

```
/students/checkins/<student_id>/
    <YYYY-MM-DD>_<CLASS_CODE>_<NN>.jpg
```

- Grouped by `<student_id>` ⇒ open one folder to review a student's history.
- `<YYYY-MM-DD>` is the **session date** (no RTC — there is no time-of-day); the
  device has no wall clock, so multiple taps the same day/class disambiguate with
  a 2-digit `<NN>` counter (next unused index in the folder).
- `<CLASS_CODE>` records which class the tap belonged to.
- ~100×100–VGA JPEG via the P4 hardware encoder (same path as `photo_store`),
  small enough that a class's worth of check-ins stays modest on the card.

Preserved by the config importer (like `/photos/**`) — never overwritten by an
import.

## Camera lifecycle (the hardware-risky part)

The camera (`face_detection_service`: OV02C10 → CSI → ISP → PPA + detection task)
is heavy and was previously crash-prone (see the `csi_pipeline` bring-up fixes).
Two options:

- **(A, chosen) Warm during a capture-enabled session.** Start the camera when
  entering the session/kiosk view *if* capture is enabled; keep it running so
  each tap verifies instantly; stop on exit. Needs a new `face_detection_stop()`.
- (B) Start per tap. Simpler lifecycle but adds CSI bring-up latency (the risky
  path) to every tap. Rejected.

**Risk:** running the camera through a whole class is the biggest unknown
(power/heat/PSRAM, and the detection task sharing cores with LVGL). This must be
validated on hardware; the code will fail *soft* (a bring-up failure disables
verification and — decision needed — either blocks or allows the tap; see Open
questions).

## Verify UI

A full-screen modal reusing `scr_camera`'s preview + box rendering:

- Title "Show your face to the camera".
- Live RGB565 preview (`face_detection_snapshot`) with the detected **bounding
  box** drawn over it (green when a face is in frame).
- A **countdown** (N…0) ring/number.
- On detect: freeze the frame, "Verified ✓", save the photo, register the tap.
- On timeout: "No face detected", reject the tap.
- The student's reference avatar / name shown for context.

## Staged plan

Each stage ends green on `pio run -e esp32p4` and `pio test -e native`.

1. **Global setting — face-verify timeout.** `config.json` `face_verify_seconds`
   (default 15) + `config_face_verify_seconds()` getter + `config_set_face_verify_seconds()`
   setter (atomic rewrite); an Admin device-settings numeric field. *Native
   tests: default/parse/clamp/persist.*
2. **Check-in photo storage (`checkin_store`).** Pure path/counter builder for
   `/students/checkins/<id>/<date>_<code>_<NN>.jpg` (native-tested) + a save that
   hardware-encodes an RGB565 frame to that path (device-only; generalizes
   `photo_store`'s encoder to a target path).
3. **Camera capture-to-path + lifecycle.** `face_detection_request_capture_to(path)`
   and `face_detection_stop()`; session/kiosk start/stop the camera when capture
   is enabled. *Device-only, hardware-risky.*
4. **Verify modal + check-in integration.** A pure verify state machine
   (`elapsed`, `face_seen` → prompt/detected/success/timeout — native-tested) +
   the LVGL modal (preview/box/countdown), wired into `on_session_card` and the
   kiosk `check_in` so a capture-enabled tap must verify before it registers,
   saving the photo on success. *Modal is device-only.*
5. **Docs + review aid.** Finalize this doc; describe reviewing
   `/students/checkins/<id>/` (an on-device or desktop viewer is out of scope).

## Native-testable vs device-only

- **Native-testable:** the `face_verify_seconds` config, the check-in path +
  counter logic, and the verify state machine (elapsed/face → outcome).
- **Device-only (build-verified):** the JPEG encode, the camera lifecycle, and
  the verify modal / preview rendering. Reported as build-verified, not run on
  hardware.

## Resolved decisions (2026-07-28)

- **Camera bring-up failure:** raise a **prominent alert** to the professor
  (banner on the session/kiosk view) with an inline **"Disable photo capture"**
  action, so they can check the device and turn capture off. While capture is on
  but the camera is down, a tap cannot be verified — it is held/rejected with a
  message pointing at the alert, until the professor disables capture (after
  which taps register normally).
- **Camera lifecycle:** warm during a capture-enabled session — start on entering
  the session/kiosk view when capture is enabled, keep it running for instant
  verification, and stop on exit (adds `face_detection_stop()`). Hardware risk to
  validate on device.
- **Manual override:** the professor's manual name-tap (force present) **bypasses
  face-verify** and saves no photo — it is an explicit staff action, consistent
  with how it already bypasses timed mode.
- **Retries:** on timeout the student simply taps again (no in-modal retry
  button).

## Changelog

- **2026-07-28** — **Stages 1–4 implemented.** Config `face_verify_seconds`
  (default 15, clamp 3–60) + Admin preset dropdown (Stage 1). `checkin_store`
  path/counter `/students/checkins/<id>/<date>_<code>_<NN>.jpg` and
  `photo_store_encode_to()` synchronous encode-to-path (Stage 2). Camera pause
  via `face_detection_stop()` (safe: keeps the pipeline warm, no deinit) +
  `photo_store_encode_to` reuse (Stage 3). The `face_verify` overlay component
  (live preview + face box + countdown) wired into the session `on_session_card`
  and kiosk `check_in`: a capture-enabled tap must show a face within the window
  before it registers, saving the preview frame as the check-in photo; the camera
  is warmed while a capture-enabled session/kiosk is open and paused on exit;
  manual name-tap bypasses; a camera-start failure raises the session alert with
  a "Disable photo capture" action (Stage 4). Native: `checkin_store` (5) +
  `photo_store_encode_to` (1) + `face_verify_seconds` (1). **Storage/config
  native-tested; camera lifecycle + verify modal build-verified only, not run on
  hardware.**
- **2026-07-28** — **scoped to kiosk only.** Removed face-verify from the main
  class Session roll-call tap (`on_session_card` registers directly again) and
  its camera warm/pause + camera-unavailable alert. Face-verified capture now
  lives solely in kiosk mode (`scr_kiosk`), which warms the camera on entry,
  pauses on exit, and gates each check-in through the verify overlay.
- **2026-07-28** — **photo check-in made class-only.** Removed the device-wide
  `capture_photos` and the global `face_verify_seconds` from `config.json` /
  `config_service` and the Admin screen (kept the camera preview). `capture_photos`
  is now a plain per-class bool and `face_verify_seconds` a per-class int in
  `class.json` (default 15, clamp 3–60), edited in the class ⚙ settings (a switch
  + a seconds field). `class_capture_enabled(cls)` is now just the class flag;
  `roster_class_update_settings` gained the face-verify param; kiosk reads
  `s_cls->face_verify_seconds`. Config-builder + CONFIG_IMPORT.md updated
  (config.json §3.1 loses the field; class.json §3.3 documents the four optional
  attendance fields). Native suite green.
