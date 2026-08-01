# SD card layout

Every path the firmware reads or writes. This is the single source of truth for
the card layout; other docs link here rather than repeating it.

The card is FAT32, mounted once at `/sdcard` by `storage/sd_card.cpp`
(`sd_card_mount()`). Paths below are relative to the card root.

## Authored — written on a laptop, imported to the device

Produced by [`tools/config-builder/`](../../tools/config-builder/) and applied by
the importer. Schemas, limits and validation rules live in
[CONFIG_IMPORT.md](CONFIG_IMPORT.md) — that document owns them.

```
/config.json                    professors: name, email, rfid_uid, numeric password
/students/students.json         global student registry: id, name, rfid_uid
/students/photos/<id>.jpg       reference avatar, 100x100 baseline JPEG
/classes/<CODE>/class.json      class metadata + roster (references student ids)
```

`<CODE>` is the class code and doubles as the folder name, so it must be a safe
path segment.

**Card ids and passwords are not readable on the card.** They are authored in the
clear, then converted on the first load into keyed fingerprints (`v1:<hex>`) and
the file is rewritten — see [CONFIG_IMPORT.md](CONFIG_IMPORT.md) §6. The key
lives in the device's NVS, never on the card.

Two consequences worth knowing before swapping cards between devices:

- A card carrying fingerprints from **another device** is not recognised: every
  student reads as unbound and no professor card or password works there. The
  data (names, classes, attendance) is fine — only the credentials are bound.
- Erasing NVS (a full flash erase, not an ordinary firmware update) has the same
  effect on that device.

Recovery either way: Admin → debug → *Delete all cards & attendance*, then
re-enrol the cards, and re-import `config.tar` for the professors.

## Device-written — created at runtime, never in a `config.tar`

```
/classes/<CODE>/attendance/YYYY-MM-DD.jsonl   one session per file, append-only
/students/checkins/<id>/<date>_<CODE>_<NN>.jpg  photo check-in evidence
/photos/IMG_nnnn.jpg                          camera-screen snapshots (.bmp fallback)
/csv_export/<CODE>.csv                        attendance export, MATRICULA,FREQ
/backup/previous/                             pre-import snapshot of the authored files
/battery.csv                                  drain log, debug builds only
```

An import **preserves** all of these — it overwrites only the authored files
above.

### Attendance JSONL

One line per presence change, last line per student wins. `min` is present only
for timed (double-tap) classes and records the measured minutes:

```
{"id":"2023-0142","present":true}
{"id":"2023-0187","present":true,"min":52}
{"id":"2023-0142","present":false}
```

Append-only is deliberate: the device has no battery-backed shutdown, so a
failed append loses at most the last tap, never the file.

### `/backup/previous/`

A single slot, overwritten on each import, mirroring the authored layout:
`config.json`, `students/students.json`, `classes/<CODE>/class.json`. It does
**not** snapshot attendance or photos — those are preserved in place and are far
too large to copy. Restoring is "import from this directory"; see
[CONFIG_IMPORT.md](CONFIG_IMPORT.md).

## Transient

```
/config.tar         dropped here to trigger an import; consumed on apply
/import_staging/    unpacked tar, validated before anything live is touched
```

## Optional — override, copied by hand

```
/models/human_face_detect_msr_s8_v1.espdl
/models/human_face_detect_mnp_s8_v1.espdl
```

**Not needed.** The face-detection models ship inside the firmware (the
`human_face_det` flash partition), so a freshly flashed device detects faces with
none of these files present. Put them here only to override the built-in copy —
trying a different model without reflashing. Both files must be present for the
override to take effect; sources are in [`models/`](../../models/), details in
[FACE_DETECTION.md](FACE_DETECTION.md).

## Preparing a new card

1. Format FAT32.
2. Drop the `config.tar` built by the config-builder at the root.
3. Insert and boot — the idle screen offers **Import config from SD**.

A complete sample tree is in [`sd_card_example/`](sd_card_example/).

### With no computer to hand

A card with no `config.json` at all leaves the device locked with no way in —
unlocking needs a professor, and the WiFi file manager that would receive a
`config.tar` sits behind the unlock gate. For that case only, the idle screen
offers **Set up this device**: create one professor with a numeric password
(`config_create_first_teacher()`), unlock with it, then upload and import a
`config.tar` from the device itself. That account has no email, so it sees every
class, and it is replaced by the first import.

Offered **only** when `config.json` is absent. A config that exists but fails to
parse has to be repaired — the device will not replace it.
