# Class Screen Revamp — plan

Reorganizes the per-class UI around a state-aware hub, moves enroll/kiosk under
an open session, removes the redundant re-auth locks, and adds a per-class
settings/statistics screen with per-class photo capture and a timed
double-tap attendance mode.

Status: **planned.** Phase 1 in progress. Device-only UI is build-verified, not
run on hardware; the timed-attendance decision logic lives in `attendance_store`
so it is native-testable.

## Locked decisions
- **Nav:** state-aware class **hub** (Session · History, plus a "Resume session"
  card when one is open) → **open-session screen** (roll call + Kiosk + Enroll).
- **Auth:** entering Kiosk/Enroll needs no password (the professor already logged
  in at idle). **Kiosk *exit* stays gated**, switched to the shared **numeric**
  keypad (passwords are digits-only). Delete the Enroll tab, the header lock
  button, and the unlock modal.
- **Per-class settings (⚙ on each class card):** statistics + per-class **photo
  capture** (overrides the device-wide flag) + **timed attendance** (on/off +
  threshold, default 45 min) + **edit name / schedule / color**.
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
- **class.json:** `capture_photos` (override; absent = inherit device global),
  `timed_attendance` (bool), `min_attendance_min` (int, default 45).
- **class_rec_t:** matching RAM fields, with an "unset ⇒ inherit global" flag for
  capture.
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
- Phases 1–2 are mostly LVGL: build-verified, not run on hardware.
- Phase 3's decision logic sits in `attendance_store` (native-tested); only the
  tap UX is device-only.
- The strict "must tap out ⇒ absent" rule leans on students tapping out — the
  timed roll call / kiosk should show a clear "tap again when you leave" hint.
