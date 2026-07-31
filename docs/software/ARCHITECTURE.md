# Architecture

How the firmware is organised, what runs on which thread, and the gotchas that
have caused real bugs. Build commands and agent rules are in
[`CLAUDE.md`](../../CLAUDE.md); the card layout is in [SD_CARD.md](SD_CARD.md).

## Layers

Strict layering, events flow up. Only `ui/` includes `lvgl.h`.

```
src/ui/        LVGL screens, components, theme
src/app/       pure logic, no hardware: event_bus, auth, session, uid, ustar,
               battery_curve, photo_fit, card_gate
src/services/  own hardware + SD, run FreeRTOS tasks: config, roster, rfid,
               battery, export, wifi_ap, file_server, face_detection, import
src/storage/   SD modules: sd_card (mount), attendance_store, photo_store,
               checkin_store, backup_store, sd_tree, battery_log
src/camera/    OV02C10 sensor, csi_pipeline, auto_exposure
src/lcd/  src/touch/  src/rfid/  src/audio/     device drivers
```

`app/` is what the native tests compile — keep new hardware-free logic there.

## Threading

There is one UI thread and several service tasks. **Services never call LVGL.**

Services post `app_event_t` to a single FreeRTOS queue (`app/event_bus`);
`main.cpp`'s `loop()` drains it on the LVGL thread through `app_dispatch()` →
`ui_handle_event()` → `lv_timer_handler()`.

**All SD writes happen on the LVGL thread.** No lock guards the card, so a
service task that writes to SD races the UI. When a service needs to persist
something, it posts an event and the drain path writes it — that is why the
battery drain log is written in `app_dispatch()` and not in the battery task
that produces the sample.

The HTTP file server is pumped from an LVGL timer for the same reason, which is
also why a large transfer briefly freezes the UI (see [BACKLOG.md](BACKLOG.md),
item C3).

## Screens

`ui/screen_manager` + `screen_t {create, on_show, on_hide}`. `create` runs once,
lazily; per-visit state belongs in `on_show`. Ids are in `include/ui/screen.h`.

| Screen | Purpose | Reached from |
|---|---|---|
| `scr_idle` | Access gate: professor card or numeric password | boot |
| `scr_classes` | The professor's class list | footer **Classes** |
| `scr_class` | One class: hub → session / history / enroll | class card |
| `scr_class_stats` | Per-class statistics + settings | ⚙ on a class card |
| `scr_kiosk` | Unattended self check-in, professor-gated exit | class session |
| `scr_export` | CSV attendance export | footer **Export** |
| `scr_wifi_editor` | Soft-AP + web file manager | footer **WiFi File Editor** |
| `scr_admin` | Profile, SD usage, password, RFID, camera, debug, import | footer **Admin** |
| `scr_camera` | Camera preview + face boxes + model diagnostics | Admin |
| `scr_about` | Credits, version, repo QR | Admin |

Footer nav appears on Classes / Export / WiFi File Editor / Admin, and is
disabled while the soft-AP is running so the AP cannot be left on by walking
away.

**Gotcha:** `create()` runs *before* `s_current` is set, so `scr_mgr_current()`
returns the *previous* screen during create.

## Class screen

`scr_class.cpp` is a **view stack**, not a tab bar — a hub with two big actions
opens into the session or history view, and enroll is reached from within an open
session. Back steps up the stack: enroll → session → hub → class list.

```
Classes ──tap──▶ Hub (Session / History, + "Resume" when a session is open)
                  ├─Session──▶ date picker  ─open─▶ roll call ──▶ Kiosk / Enroll
                  └─History──▶ past sessions with attendance %
```

Entering kiosk or enroll needs no password: the professor is already signed in,
and kiosk has its own exit gate.

### Check-in modes

Per class, from `class.json` (schema in [CONFIG_IMPORT.md](CONFIG_IMPORT.md)):

- **Single tap** — one tap marks the student present.
- **Double tap** (`timed_attendance`) — tap on arrival, tap again once
  `min_attendance_min` has passed. A second tap *before* the threshold records
  nothing and reports the minutes remaining, so the arrival still stands and the
  student can come back. Once registered, further taps are ignored. Elapsed time
  comes from the monotonic clock, so the in-progress state is RAM-only and a
  reboot voids it.
- **Photo check-in** (`capture_photos`) — kiosk taps face-verify before
  registering; see [FACE_CHECKIN.md](FACE_CHECKIN.md).

The device treats the two flags independently and supports combining them; the
config-builder authors one mode at a time.

### Roll call

Rendered alphabetically, filtered by a search box, and capped at `ROLL_MAX_ROWS`
chips with a "Showing N of M" header — a chip costs ~5 LVGL objects and a large
class would otherwise exhaust the pool. Taps rebuild only the chip container via
`update_roll_call()`, not the whole view.

## Reactive state and card capture

`ui/ui_state.cpp` exposes `lv_subject` observables (WiFi RSSI, AP active, battery
% and mV, config/roster status, user name) that the status bar and screens bind
to.

`ui_set_card_capture(cb)` diverts the next scanned card to a flow such as enroll.
It is **one-shot** — the dispatcher clears it before calling, so a flow that
wants the next card must re-arm. Nothing re-arms it implicitly.

### Reading cards

The PN532 is polled for **one** target at a time (`InListPassiveTarget` with
MaxTg=1), so when several cards sit on the reader it returns whichever wins
anticollision — and that **alternates between polls**. A debounce that only
remembers the previous UID is therefore useless: every poll looks like a new
card. `app/card_gate` handles this by requiring the same UID on two consecutive
polls before accepting one (alternating cards never manage a run that long), and
by treating a UID that reappears after a different one as proof that more than
one card is present — reported once as `APP_EVENT_CARD_COLLISION` so the UI can
ask for a single card. Nothing registers until the reader is cleared.

## LVGL gotchas

These have each caused a real bug:

- **`keyboard_hide()` before any `lv_obj_clean()` that deletes a textarea.** The
  shared keyboard holds a pointer to its target; cleaning the widget without
  hiding it first leaves a dangling `->ta` that the next `keyboard_hide()`
  dereferences. See `enroll_goto()` and `rebuild_content()`.
- **Modals with textareas belong on the screen root, not `layer_top`,** so the
  shared keyboard floats above them.
- **Overlays parented to the shell root need `LV_OBJ_FLAG_IGNORE_LAYOUT`,**
  otherwise they are laid out as flex children and misrender.
- **Watch the LVGL heap.** `LV_MEM_SIZE` is 256 KB and `LV_USE_ASSERT_MALLOC=1`
  turns exhaustion into a silent infinite loop on the UI thread — a freeze with
  no panic and no reboot. Prefer updating changed widgets over full rebuilds on
  large lists.
- **`lv_image` reports its untransformed size** as its self-size, so a scaled
  image must also be `lv_obj_set_size`d to its drawn size or the layout reserves
  the wrong box. See `ui/components/student_photo.cpp`.

## Theme

`ui/theme/theme.cpp` — light "staff" palette plus a dark palette for the idle and
kiosk screens, with `ui_make_button/label/card` helpers and `ui_add_press_feedback`
(opacity dim + tick beep). Beeps are `beeper_touch` (tick), `beeper_beep` (grant),
`beeper_error` (two-tone).

Fonts are Montserrat with FontAwesome glyphs merged in
(`src/ui/assets/font_montserrat_custom_{14,20,32}.c`); see
[CUSTOM_FONT_GENERATION.md](CUSTOM_FONT_GENERATION.md). One shared keyboard lives
on `layer_top` with a reduced numeric keymap.

## Data model

Students are a **global registry** keyed by university id; classes reference
students by index, and each student has one record and at most one card binding.
`roster_service` loads and strictly validates the tree, and writes atomically
(measure → heap buffer → temp file → remove + rename).

Validation of a staged import reuses the same loader under a single lock hold:
load from the staging root, capture the result, reload from the live root to
restore, then release. Because it never releases the lock in between, no other
task observes the staging data — and it avoids a second copy of the ~80 KB
roster arrays.
