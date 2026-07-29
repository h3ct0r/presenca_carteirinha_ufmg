# Class Screen Revamp — plan

Reorganizes the per-class UI around a state-aware hub, moves enroll/kiosk under
an open session, removes the redundant re-auth locks, and adds a per-class
settings/statistics screen with per-class photo capture and a timed
double-tap attendance mode.

Status: **all three phases implemented** (build- and native-test-verified, not
run on hardware). The timed-attendance decision logic lives in `attendance_store`
and is fully native-tested; the screens/tap UX are device-only.

## Locked decisions
- **Nav:** state-aware class **hub** (Session · History, plus a "Resume session"
  card when one is open) → **open-session screen** (roll call + Kiosk + Enroll).
- **Auth:** entering Kiosk/Enroll needs no password (the professor already logged
  in at idle). **Kiosk *exit* stays gated**, switched to the shared **numeric**
  keypad (passwords are digits-only). Delete the Enroll tab, the header lock
  button, and the unlock modal.
- **Per-class settings (⚙ on each class card):** statistics + per-class **photo
  check-in** (on/off + face-verify seconds; class-only, no device-wide flag) +
  **timed attendance** (on/off + threshold, default 45 min) + **edit name /
  schedule / color**.
- **Timed attendance:** two taps — tap in (arrival) then tap out (leaving).
  Present **iff** `out − in ≥ threshold` **and** the student tapped out; a student
  who taps in but never taps out is **absent** at session close. **Store the
  measured minutes.** Duration comes from the monotonic uptime
  (`esp_timer_get_time`), which measures elapsed time fine without an RTC.
  In-progress (tapped-in-only) state is **RAM only**, so a reboot mid-session
  voids in-progress measurements (inherent to having no RTC — documented, not a
  bug). Manual name-tap by the professor stays a force present/absent override.

## Navigation map
```
Classes list ─tap card──▶ Class hub  (Resume? · Session · History)
      └─tap ⚙───────────▶ Class Statistics & Settings
Hub ─Session─▶ date picker ─open─▶ Open-Session screen
                                   (roll call + stats + [Kiosk] [Enroll])
Open-Session ─Kiosk──▶ Kiosk (exit = numeric password)
Open-Session ─Enroll─▶ Enroll (no unlock; search → manual + turma → tap card)
```

## Data-model changes (all optional → existing cards unaffected)
- **class.json:** `capture_photos` (bool), `face_verify_seconds` (int, default 15,
  clamp 3–60), `timed_attendance` (bool), `min_attendance_min` (int, default 45).
  Photo check-in is class-only — there is no device-wide capture flag.
- **class_rec_t:** matching RAM fields.
- **attendance JSONL:** optional `"min"` — `{"id":"…","present":true,"min":52}`.
  Old readers ignore it; the present-bool fold is unchanged.

## Service additions
- **roster_service:** load the 3 new class fields;
  `roster_class_update_settings(dir, name, schedule, color, capture, timed,
  min_min)` — atomic temp→rename, **preserving roster + turma** (DOM edit).
- **config precedence:** `class_capture_enabled(cls)` = class value if set, else
  `config_photo_capture_enabled()`.
- **attendance_store (native-testable core):**
  - `attendance_tap(id, now_us, threshold_min)` — no tap-in ⇒ record tap-in
    (RAM); has tap-in ⇒ finalize (`min = (now − in)/60e6`; present iff
    `min ≥ threshold`, persist `present` + `min`; else *left-early*, not present).
  - `attendance_tap_state(id)` → `{absent | in_progress(min_so_far) |
    present(min) | left_early(min)}` for the roll call.
  - Manual name-tap = force present/absent override (bypasses timing).

## Removed
`TAB_ENROLL`, `s_lock_btn` + `update_lock_state`, `open_unlock_modal` /
`on_unlock_card` / `unlock_pw_ok_cb` / `s_unlocked`. Header simplifies to
back/title, with a context-aware back (deeper view → hub → classes list).

## Phased implementation (each ends green on `pio run` + `pio test -e native`)
1. **Nav + auth reorg** *(pure UI, device-verified)* — hub, open-session view,
   Kiosk/Enroll buttons, remove the unlock lock, kiosk-exit → numeric keypad.
2. **Per-class settings screen** — ⚙ entry; stats (present/photo/turma +
   attendance-% aggregates), capture toggle, name/schedule/color editing; adds
   `roster_class_update_settings` (native tests: class.json round-trip, capture
   precedence).
3. **Timed attendance** — class.json fields; the `attendance_store` tap state
   machine + finalize (native tests: `in→out ≥ 45 ⇒ present w/ min`,
   `< 45 ⇒ absent`, `in-only ⇒ absent at close`, minutes persisted); roll-call
   states + stats/export surfacing minutes.

## Honesty / risk
- Phases 1–3 have been **manually verified on the device** and work as expected.
  The changelog entries below record what was verified when each landed.
- Phase 3's decision logic sits in `attendance_store` (native-tested); only the
  tap UX is device-only, and that was checked by hand.
- The strict "must tap out ⇒ absent" rule leans on students tapping out — the
  timed roll call / kiosk should show a clear "tap again when you leave" hint.

### Changelog

- **2026-07-28** — **Phase 3: timed (double-tap) attendance.** `attendance_store`
  gained the tap state machine (`attendance_tap`, `attendance_tap_state`) — first
  tap = arrival, second = leave, present iff `leave − arrive ≥ threshold`, with
  a monotonic clock (no RTC) and RAM-only in-progress state. Finalized results
  write `{"present","min"}` to the same JSONL (backward-compatible fold).
  `class.json` gained `timed_attendance` + `min_attendance_min` (default 45),
  surfaced as a switch + minutes field in the ⚙ settings screen. Session
  roll-call, the check-in overlay, and kiosk now branch on timed mode
  (in-progress / present-N-min / left-early feedback). Native: +5
  `test_attendance` cases + a `test_roster` round-trip. **Build- and
  native-test-verified, not run on hardware.**
- **2026-07-28** — **Phase 2: per-class statistics & settings screen**
  (`scr_class_stats`, `SCREEN_CLASS_STATS`), opened from a ⚙ button on each class
  card. Shows student/photo/turma/attendance-rate stats and edits name, schedule,
  color (swatch picker), and a per-class **photo-capture override** (inherit /
  on / off). Added `class_rec_t.capture_photos`, `class_capture_enabled()`, and
  `roster_class_update_settings()` (atomic class.json rewrite preserving roster +
  turma). Native: `test_class_capture_and_settings`. Later fix: the screen
  scrolls (inner container no longer captures the gesture) and a keyboard-height
  bottom pad keeps focused fields clear of the on-screen keyboard.
- **2026-07-28** — **Phase 1: navigation + auth reorg.** The class screen went
  from a 3-tab bar (Session / History / Enroll) to a **view stack**: a state-aware
  hub (Session · History, plus a "Resume session" card and a header "session
  open" chip), an open-session view carrying **Kiosk** and **Enroll** buttons, and
  a context-aware back. Removed the enroll re-auth (`s_unlocked` + unlock modal +
  header lock button); Kiosk/Enroll are entered without a password (the professor
  is already logged in), while the **kiosk *exit*** keeps its gate — switched to
  the shared **numeric** keypad. Exiting kiosk returns into the running session,
  and student photos now appear on the kiosk confirmation too. Pure LVGL —
  build-verified, not run on hardware.
- **2026-07-29** — **kiosk exit: a professor card tap now leaves immediately.**
  `on_kiosk_card` checks `auth_lookup_uid()` **before** the student lookup, so a
  professor tapping their RFID anywhere in kiosk calls `do_exit()` directly —
  no Exit button, no gate modal, no keypad. Previously that tap fell through to
  the student lookup and showed "Invalid card". The Exit button + modal remain as
  the fallback for a professor with no bound card (or one not to hand), and the
  security property is unchanged: the professor card was already sufficient to
  clear the gate, this only removes the extra step. The professor-first ordering
  also means a professor who is *also* enrolled as a student exits rather than
  checking themselves in. Note a card tap is ignored while the face-verify
  overlay is up (capture is disarmed there), so the professor taps again after it
  times out. Build- and native-test-verified, not run on hardware.
