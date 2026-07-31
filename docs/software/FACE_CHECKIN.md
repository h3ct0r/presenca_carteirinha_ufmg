# Face-verified check-in + per-student check-in photos

When a class enables photo check-in (a per-class option; there is no device-wide
flag), a kiosk check-in is only accepted after a **face is detected on camera
within a countdown window**, and the captured frame is saved on the SD card
**grouped by student** so a reviewer can later flip through a student's check-in
photos and confirm the same person tapped every time.

Status: **shipped and verified on hardware.** The storage and settings logic is
native-tested; the camera lifecycle and verify modal are device-only and were
exercised by hand. The changelog records what was verified when each change
landed.

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
  class roll call registers attendance directly (no camera). Rationale:
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
  in timed mode both the arrival tap and the confirming tap are verified (a photo
  each). The verify modal is the same either way. Note that verification runs
  *before* the tap is judged, so a tap rejected as too early (below the class
  threshold) still leaves a photo behind — the extra shot is evidence the student
  came by, and the `NN` counter keeps same-day files distinct.

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
- The saved frame is the 480×270 preview the detector ran on, JPEG-encoded by the
  P4 hardware encoder (same path as `photo_store`) — small enough that a class's
  worth of check-ins stays modest on the card.

Preserved by the config importer (like `/photos/**`) — never overwritten by an
import.

## Camera lifecycle

The camera (`face_detection_service`: OV02C10 → CSI → ISP → PPA + detection task)
is heavy and was crash-prone during bring-up, so it is **warmed for the whole
capture-enabled session** rather than started per tap: entering a kiosk for a
capture-enabled class starts it, each tap then verifies without paying CSI
bring-up latency, and leaving stops it. Starting per tap was considered and
rejected for that latency.

Running the camera through a whole class was the main unknown (power, heat,
PSRAM, and the detection task sharing cores with LVGL); it has since been
exercised on device. Bring-up failure fails *soft* — verification is disabled and
the tap is allowed.

## Other decisions

- **Manual override:** the professor's manual name-tap (force present) **bypasses
  face-verify** and saves no photo — it is an explicit staff action, consistent
  with how it already bypasses timed mode.
- **Retries:** on timeout the student simply taps again (no in-modal retry
  button).
- **Camera bring-up failure** was specified to raise a banner on the session view
  with an inline "Disable photo capture" action. **That banner was never built** —
  today a bring-up failure just disables verification silently and taps register
  normally.

## Changelog

- **2026-07-29** — **shutter cue + 1 s photo review on capture.** The verify
  overlay used to call `finish(true)` on the same tick the face was detected, so
  the capture was invisible — the student never saw the shot (nor even the green
  "detected" state, which was set and then immediately torn down). Now, on
  detect: the preview **stops refreshing first** (so `s_buf`, and therefore the
  image on screen, is exactly the frame being written), the photo is encoded, a
  white sheet flashes to 80% and fades out over 280 ms (`lv_anim`, ease-out), and
  the frozen shot is held for **1 s** before the tap registers. The status pill
  reports **"Photo saved" / "Photo not saved"** (a failed encode now also logs,
  instead of being silently ignored), and the countdown ring stops. The flash
  sheet is a child of the overlay with `LV_OBJ_FLAG_IGNORE_LAYOUT` — the overlay
  is a flex column, so without it the sheet would be laid out as another child;
  its animation is explicitly deleted in `take_down()` so it can never tick on a
  freed object. Build- and native-test-verified, and **verified on hardware
  (2026-07-29).**
- **2026-07-28** — **Verify overlay UI polish.** Reworked the `face_verify`
  overlay layout for a more professional kiosk look on the 480×800 portrait
  screen: full-bleed rounded/clipped **viewfinder** (thin border, top corners
  blend into the deep bg) pinned to the top; a controls block that grows to fill
  and centers its content in the remaining height (no more top-heavy dead space);
  the bare countdown number replaced by a **circular countdown ring** (`lv_arc`)
  with the seconds in its center; the muted status line replaced by a **status
  pill** that turns green with a ✓ on detect; and a **student avatar + name**
  context row (reuses `student_photo`, placeholder when no reference photo). Face
  boxes still render 1:1 in preview pixels; tick/detect/timeout logic unchanged.
  Build- and native-test-verified; **not run on hardware.**
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
