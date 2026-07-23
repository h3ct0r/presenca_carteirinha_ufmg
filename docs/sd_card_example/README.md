# SD card layout

Copy this folder's contents to the root of a FAT32 SD card. The device
validates the layout at boot and shows the exact problem on screen if a file
is missing or malformed.

```
/config.json                    teachers allowed to unlock, each with their own password
/students/
    students.json               global student registry (one entry per student)
    photos/                     avatars, named <university id>.jpg (~240x240)
/classes/
    <CLASS-CODE>/
        class.json              class metadata + roster (university ids)
        attendance/             per-session logs, created by the device
            YYYY-MM-DD.jsonl    append-only, one line per presence change:
                                {"id":"2023-0142","present":true}
/models/                        ESP-DL face-detection models (only needed if the
    human_face_detect_msr_s8_v1.espdl   camera / face detection feature is used)
    human_face_detect_mnp_s8_v1.espdl
```

Attendance is written by the device (Session tab): pick a date, open the
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
