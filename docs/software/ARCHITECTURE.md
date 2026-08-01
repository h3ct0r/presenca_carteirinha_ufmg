# Architecture

How the firmware is organised, what runs on which thread, and the gotchas that
have caused real bugs. Build commands and agent rules are in
[`CLAUDE.md`](../../CLAUDE.md); the card layout is in [SD_CARD.md](SD_CARD.md).

## Layers

Strict layering, events flow up. Only `ui/` includes `lvgl.h`.

```
src/ui/        LVGL screens, components, theme
src/app/       pure logic, no hardware: event_bus, auth, session, uid, ustar,
               battery_curve, photo_fit, card_gate, class_stats
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

**SD writes belong on the LVGL thread.** No application lock guards the card, so
a service task that writes to SD races the UI. When a service needs to persist
something, it posts an event and the drain path writes it — that is why the
battery drain log is written in `app_dispatch()` and not in the battery task
that produces the sample.

Three places knowingly break that rule and touch the card from their own task:
`photo_store`'s writer task, the `config`/`roster` retry loops, and the HTTP
file server. What keeps them safe is FatFs itself — the ESP-IDF build sets
`FF_FS_REENTRANT`, so the volume is locked for the duration of each operation
and concurrent access cannot corrupt the filesystem. What it does *not* buy is
logical consistency: a file rewritten from one task while another reads it can
be seen half-written. That is the reason the rule stands for everything else.

The file server is the deliberate exception (`services/file_server`): it is a
synchronous `WebServer`, so serving it from an LVGL timer froze the UI for the
length of every transfer. It runs on its own `fileserv` task instead.

By default the footer nav is locked while the AP is up, so the browser can only
change the card while the professor watches one screen. The debug switch on
`scr_wifi_editor` lifts that lock (off at every boot, never persisted), and then
the card can change under any screen. Two rules keep that honest:

- the server counts its writes (`file_server_write_count()`), and `ui/sd_resync`
  turns "the count moved" into dropped caches: `ui_sd_resync_light()` for the
  memoised attendance counts — safe anywhere — and `ui_sd_resync_full()`, which
  adds a config + roster reload and is called **only** from the class list and
  the idle gate. `roster_service_reload()` rewrites the storage `scr_class`
  holds a `class_rec_t*` into, so reloading under an open class screen would
  retarget it;
- returning to the idle gate calls `scr_wifi_editor_stop_ap()`, so signing out
  cannot leave an unauthenticated file manager on air;
- the AP's memory cost is permanent for the boot — `wifi_ap_stop()` keeps the
  stack up on purpose — and it comes out of the internal pool the face model's
  SD read needs. That used to leave too little and panic esp-dl; since the LVGL
  pool moved to PSRAM there is room for both, and the camera service decides on
  free memory rather than on whether WiFi ran (see
  [FACE_DETECTION.md](FACE_DETECTION.md) §Status / caveats).

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
away — unless the debug background switch is on (see §Threading).

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

### Cost of listing past sessions

The history view and the statistics screen each want a present-count for every
recorded date, and each count is a whole-file fold of that day's JSONL.
`attendance_present_for()` therefore **memoises** per (class, date); writes
through `attendance_store` drop the affected day, and
`attendance_history_cache_clear()` exists for the one case that bypasses the
module — the debug WiFi file editor. Without it, a class with 24 sessions cost
24 blocking SD reads on every rebuild of either view.

The open-a-session picker asks for one date only: it shows the **last** session
as a shortcut (`s_dates[0]` — `attendance_list_dates()` is newest first) and
leaves the full list to History, which is also what keeps a scrollable box out
of the already-scrolling body.

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

## Memory budget

Two pools, and only one of them is scarce.

**Internal RAM — 768 KB** (`0x4ff00000`–`0x4ffc0000`), and the only place
DMA-capable buffers can come from. `sdmmc` needs one for **every** transfer, so
when this runs out the whole SD card goes away at once: reads fail with
`ESP_ERR_NO_MEM` (`0x101`), attendance and config read as empty rather than
erroring, and if the WiFi stack is up ESP-Hosted eventually cannot allocate a TX
buffer and asserts in `transport_drv_ap_tx`. That is the failure signature to
recognise — a wall of `sdmmc_read_sectors: not enough mem` is a memory report,
not a card problem.

The permanent occupants, measured with `nm --size-sort` on `firmware.elf`:

| What | Size | Where |
|---|---|---|
| LVGL heap (`LV_MEM_SIZE`) | 512 KB | **PSRAM** via `LV_MEM_POOL_ALLOC` |
| LVGL draw buffers (2 × 480×800×2) | 1.5 MB | PSRAM |
| `s_students` (600 × `student_t`) | 53 KB | internal `.bss` |
| `s_classes` (12 × `class_rec_t`) | 28 KB | internal `.bss` |
| everything else static | ~47 KB | internal `.bss` |
| task stacks (loopTask 16 K, face_detect 16 K, fileserv 8 K, …) | ~68 KB | internal |
| WiFi + ESP-Hosted, once started | tens of KB | internal, **never freed** |

LVGL's pool is the reason `LV_MEM_POOL_ALLOC` exists in `lv_conf.h`: left to
itself LVGL declares it as a static array, which put 256 KB of the 768 KB out of
reach and left too little for SD DMA once WiFi came up. Static internal use is
128 KB with it in PSRAM, against 384 KB before — and the pool has since been
doubled to 512 KB, which costs PSRAM only (TLSF's control block lives inside the
pool, and `FL_INDEX_MAX` sizes itself from `LV_MEM_SIZE`). `update_roll_call()`
logs a line whenever the pool hits a new high-water mark, which is the number to
size it from; the boot figure in `main.cpp` is measured before any screen is
built and says nothing about the worst case.

Note what `wifi_ap_stop()` does *not* do: it drops the visible network but keeps
the stack and the P4↔C6 link, so its memory is gone until a reboot. Three places
log the pool so this stays measurable — `main.cpp` at boot, `wifi_ap` around
start/stop, and `face_detection_service` around the model load.

**Task stacks are the other way to run out.** A service task gets a few KB, and
two things eat it fast: a `device_config_t`/roster struct copied onto the stack
(~1.5 KB each) and newlib's `vfprintf`, whose frame is **1152 bytes** — so any
`ESP_LOG*` or `snprintf` on a deep path costs more than a kilobyte. That
combination overflowed the 5 KB `config` task on every boot with no `config.json`
(a *stack-protection panic*, which reports a task and an address but no message):
`load_config` and `parse_config_file` each held a `device_config_t`, and the
failure path logged from the bottom of both. `load_config` now heap-allocates
its copy. When a task is near its limit, prefer moving big structs to the heap
over growing the stack — internal RAM is the scarce pool. `config_task` logs its
own high-water mark once, which is how to size it rather than guess.

**PSRAM — 32 MB**, comfortable. Draw buffers, the LVGL heap, camera preview
buffers, decoded avatars (20 KB each). Nothing here has been close to the limit.

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
- **Watch the LVGL heap.** `LV_MEM_SIZE` is 512 KB (in PSRAM, see §Memory
  budget) and `LV_USE_ASSERT_MALLOC=1` turns exhaustion into a silent infinite
  loop on the UI thread — a freeze with no panic and no reboot. Prefer updating
  changed widgets over full rebuilds on large lists.
- **`lv_image` reports its untransformed size** as its self-size, so a scaled
  image must also be `lv_obj_set_size`d to its drawn size or the layout reserves
  the wrong box. See `ui/components/student_photo.cpp`.
- **A GT911 read answers "is there a new report?", not "is a finger down?"**
  `esp_lcd_touch_gt911_get_xy()` consumes each report (`tp->data.points = 0`),
  and `read_data()` only refreshes it when the panel has published since the last
  poll. Sampling the indev faster than the panel's refresh cadence therefore
  turns the gaps into phantom releases — one tap arriving as two clicks. The
  indev is read every 10 ms (for scroll smoothness), so `gt911_touch::getTouch()`
  holds the last point for `TOUCH_HOLD_US` before reporting a release.

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
