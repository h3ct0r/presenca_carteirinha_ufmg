# Sample SD card

A minimal working card. Copy this folder's contents to the root of a FAT32 SD
card. The device validates the layout at boot and shows the exact problem on
screen if a file is missing or malformed.

```
/config.json                    teachers allowed to unlock, each with their own password
/students/
    students.json               global student registry (one entry per student)
    photos/                     avatars, named <university id>.jpg (optional)
/classes/
    <CLASS-CODE>/
        class.json              class metadata + roster (university ids)
/models/                        ESP-DL face-detection models (only needed if the
    human_face_detect_msr_s8_v1.espdl   camera / face detection feature is used)
    human_face_detect_mnp_s8_v1.espdl
```

These are the files you author. The device creates everything else itself —
attendance logs, check-in photos, CSV exports, backups — and an import never
touches them. The complete card layout is in [SD_CARD.md](../SD_CARD.md).

Rather than editing this tree by hand, most users author a `config.tar` with
[`tools/config-builder/`](../../../tools/config-builder/) and import it; see
[CONFIG_IMPORT.md](../CONFIG_IMPORT.md).

Attendance is written by the device: open a class, pick a date, open the
session, then tap cards or names to mark students present. Each change is one
appended JSONL line — the current state is the fold of the file (last line
per student wins), which is safe against the device being switched off
mid-class. There's no clock on the board, so the date is chosen with the
picker (it defaults to the last recorded session date).

Notes:

- Each teacher in `config.json` has their own `password` (the fallback when
  their RFID tag isn't handy). Passwords must be **different** for every
  teacher — the device refuses to start otherwise, since a password identifies
  exactly one professor. They must also be **digits only** (entered on a
  numeric keypad); a non-numeric password stops the device with an error.
- `students.json` is the only place a student's RFID card binding lives.
  `"rfid_uid": null` means "not matched yet" — the professor binds it the
  first time the student taps their card in class.
- Class rosters reference students by their university `id`. Every id in a
  roster must exist in `students.json` (validated at boot).
- To add a late-enrolling student by hand: add them to `students.json`, then
  add their id to the class's `roster`.
- A student may appear in any number of class rosters; the card binding and
  photo are shared automatically.
- `"turma"` is an optional class-group tag (e.g. `"M1"`, `"TE1"`) on each
  **`class.json` roster entry** — `{ "id": ..., "turma": ... }`. It's filled by
  the config-builder's *Diário de Classe* CSV import. It lives on the roster
  entry (not the global student registry), so one class can hold students from
  different turmas and a student can carry a different turma per class (Maria is
  `M1` in CS101 and `F1` in MA110). See [CONFIG_IMPORT.md](../CONFIG_IMPORT.md) §3.3.
- `class.json` also accepts optional per-class check-in settings
  (`capture_photos`, `face_verify_seconds`, `timed_attendance`,
  `min_attendance_min`). They are omitted here, so this sample uses the defaults:
  single-tap check-in, no photo verification.
