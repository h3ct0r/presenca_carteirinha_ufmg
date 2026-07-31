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

## Optional — copied by hand

```
/models/human_face_detect_msr_s8_v1.espdl
/models/human_face_detect_mnp_s8_v1.espdl
```

Required only for face detection and photo check-in. A missing model is handled:
the camera runs preview-only rather than faulting. Staged copies are in
[`sd_card_example/models/`](sd_card_example/models/); see
[FACE_DETECTION.md](FACE_DETECTION.md).

## Preparing a new card

1. Format FAT32.
2. Copy `/models/*.espdl` (only if using the camera).
3. Drop the `config.tar` built by the config-builder at the root.
4. Insert and boot — the idle screen offers **Import config from SD**.

A complete sample tree is in [`sd_card_example/`](sd_card_example/).
