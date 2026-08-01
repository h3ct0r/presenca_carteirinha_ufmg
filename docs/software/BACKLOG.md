# Backlog

Known-open work. Every item here was re-verified against the code on 2026-07-31;
anything that turned out to be already done was removed rather than left with a
stale status.

## Correctness / stability

**Accented characters do not render.** Every font in the build covers `0x20-0x7F`
only — both the stock `lv_font_montserrat_*` and the custom
`font_montserrat_custom_*`. Latin-1 Supplement (`0xC0-0xFF`) is missing, so
accented Portuguese names imported from a Diário CSV draw as blanks or boxes. The
importer decodes them correctly; the font cannot draw them. Read straight off the
glyph tables (`range_start = 32, range_length = 95`), not inferred — it has gone
unnoticed because the sample rosters are ASCII-only. The fix and a ready-to-run
`lv_font_conv` command are in the "Known gap" callout of
[CUSTOM_FONT_GENERATION.md](CUSTOM_FONT_GENERATION.md).

**Internal RAM is still the constrained pool, and the roster caps now dominate
it.** Moving the LVGL heap to PSRAM returned 255 KB, and moving
`attendance_clear`/`attendance_present_for`/`load_students` scratch off `.bss`
returned another 8 KB — but raising `ROSTER_MAX_CLASS_STUDENTS` to 200 then spent
26,808 B, so static internal is back up to 160 KB. Of that, **104 KB** is
`s_students` (55,200 B) and `s_classes` (51,216 B): fixed-size arrays holding the
600-student / 12-class / 200-per-class caps regardless of what the card actually
contains.

Two levers, in order of value:
- **Move `s_students`/`s_classes` to PSRAM** at `roster_service_start()`. Returns
  ~104 KB and removes the coupling between "how many students can we hold" and
  "is there room for the face model". This is the one to take before raising any
  cap again.
- **`class_rec_t::roster_turma[200][16]` is 3,200 B of the 4,268 B per class**
  — 38 KB across 12 classes for a tag ARCHITECTURE.md describes as
  "display/statistics only". Storing turmas as indices into a small per-class
  string table, or dropping the in-RAM copy entirely, would reclaim most of it.

See [ARCHITECTURE.md](ARCHITECTURE.md) §Memory budget for what the pool is spent
on and how to measure it.

**The face-model memory thresholds are set by argument, not measurement.**
`MIN_INTERNAL_FREE` (128 KB) and `MIN_INTERNAL_BLOCK` (64 KB) in
`face_detection_service` were chosen as a comfortable margin over the one
observed failure (46,976 B total free), not from a run that measured how much
the model's SD read actually needs. The `log_heap()` lines in `wifi_ap` and
`face_detection_service` print both numbers at every camera start and around the
load; one session with the AP up settles whether these are too tight, too loose,
or right.

**Q4 — battery % vs real discharge.** The curve is linear **4.20 V (100%) → 2.70 V
(0%)** in 75 mV / 5% steps (pack datasheet range; post-`BAT_CAL_SCALE` volts).
The ADC divider and the shape under load are still unverified against a real
discharge — the `BATTERY_DRAIN_LOG` build flag exists to collect that data.

## Security

**S1 — the web file manager has no authentication.** Plain HTTP, full read/write
over the soft-AP, and unauthenticated `/api/upload` anywhere on the card.
Credentials are no longer part of the exposure — card ids and passwords are keyed
fingerprints now (ARCHITECTURE.md §Stored credentials), so `/api/read` on
`config.json` yields nothing replayable — but everything else on the card is
still readable, writable and deletable by anyone on the AP. Recursive
folder delete widens the blast radius without changing the kind of exposure. The
AP is per-boot, WPA2-protected and professor-started, which is why this is
tolerated for a debug tool — it is the reason to add auth, not a new hole.
The background switch (`scr_wifi_editor`) widens the window: the AP can now stay
up while the professor is on another screen, so nobody is necessarily looking at
it. It is bounded on both sides — off at every boot, never persisted, and the
idle gate stops the AP on sign-out — but it is one more reason to add auth.

**S2 — neither debug wipe takes a backup.** "Delete all cards & attendance" and
"Erase the whole SD card" are immediate and unrecoverable, gated only by the
debug toggle and a confirm modal. `backup_store_create()` exists and aborts its
caller on failure, but its only caller is the importer. For the full-card wipe a
snapshot would be pointless anyway — `/backup` lives on the card being erased. An
off-device copy is the only real undo.

## Code quality

**Q1 — `scr_class.cpp` is a god-screen** at roughly 1300 lines: hub, date picker,
roll call, history, enroll and the feedback overlay. Splitting the enroll flow out
is the obvious first cut.

## UX

**U3 — the "Export Selected" button scrolls with the list** instead of staying
pinned, so with many classes it can be off-screen when you want it.

**U4 — CSV `MATRICULA` is the student id verbatim,** dashes and all. Fixed by
contract with whoever consumes the file; noted so it is not "fixed" by accident.

## Deferred by decision

**Check-in photo review aid.** There is no way to browse
`/students/checkins/<id>/` on the device to audit "same person every tap" — it is
reviewed by opening the folder off-device. An on-device gallery or a desktop
viewer is wanted eventually.

**Verify state-machine test.** The countdown/detect logic is inlined in the
`face_verify` modal and is therefore device-only. Extracting it to a pure,
native-tested helper was planned and skipped as low-value; revisit if it grows.

**On-device config-builder.** Serving the browser-only builder from the device
over the soft-AP. Designed, measured and costed, not built — see
[ONBOARD_CONFIG_BUILDER.md](ONBOARD_CONFIG_BUILDER.md).

**Streaming tar unpack.** The importer reads the whole `config.tar` into one
PSRAM buffer (16 MB cap) rather than streaming it to a staging file.

**Orphan photo cleanup.** Removing a student leaves
`/students/photos/<id>.jpg` behind; an import overwrites but never deletes.

## Hardware

3D CAD details to check on the next revision: the RFID antenna offset, and the
USB connector position relative to the enclosure cutout.
