# CSV Export

Exports per-class attendance to CSV files on the SD card, one file per class.
The feature is reachable from the **CSV Export** button in the bottom navigation
bar (between **Classes** and **Admin**).

## What it produces

For each class, a file is written to:

```
/csv_export/<class code>.csv
```

The file name is the class code (e.g. a class with code `CS101-M1` exports to
`/csv_export/CS101-M1.csv`). The `/csv_export/` folder is created automatically
on the first export.

### CSV format

```
MATRICULA,FREQ
202500001,2
202500002,1
202500003,0
```

- **`MATRICULA`** — the student's university id, exactly as stored in the
  registry (`/students/students.json`). It is written verbatim, so if an id
  contains separators (e.g. `2025-0001`) they appear in the file as-is.
- **`FREQ`** — the number of **session days the student was absent** across the
  class's entire attendance history.

There is exactly one row per student enrolled in the class, in roster order,
plus the header line.

### How FREQ (absences) is computed

For each class the exporter reads every recorded session date under
`/classes/<code>/attendance/YYYY-MM-DD.jsonl` and, for every enrolled student,
counts the days on which that student was **not** present:

```
FREQ(student) = (number of session dates) − (session dates the student was present)
```

A student with no attendance record on a given date counts as absent for that
date. A class with no recorded sessions yields `FREQ = 0` for everyone.

> Presence per date is the fold of that day's append-only log (last write per
> student wins), the same rule the roll-call and history screens use.

## Using the screen

Each class is shown as a card with:

- **Number of students** — enrolled students in the class.
- **Number of days** — how many session dates have been recorded.
- **Start → end dates** — the earliest and latest recorded session date (or
  "No sessions recorded yet" when there is no attendance).
- An **Export CSV** button.

Only the classes assigned to the **logged-in professor** are listed.

### Export flow

1. Tap **Export CSV** on a class card.
2. If no export file exists yet for that class, it is written immediately.
3. If a file already exists, an **Overwrite / Cancel** dialog appears first.
   - **Overwrite** replaces the file (it is truncated and rewritten, never
     appended to).
   - **Cancel** aborts and leaves the existing file untouched.
4. On success, the card's status line shows the saved path and file size, e.g.

   ```
   ✓ /csv_export/CS101-M1.csv (1.37 kB)
   ```

   On failure, the status line and a toast show the reason (e.g. the SD card
   could not be written).

Cards that already have an export file also show a hint
("A previous export exists — Export will ask to overwrite.") before you tap.

## Behavior notes

- **Open roll call is preserved.** Producing the CSV opens each historical date
  internally to tally absences. If a roll-call session is in progress when you
  export, it is snapshotted and restored afterward, so the in-progress session
  is not disturbed.
- **Overwrite is a full replace**, so the file always reflects the current
  attendance history — no stale rows or duplicated headers.
- Exports are capped at one school year of session dates per class (366 days).

## Where it lives (for developers)

| Piece | File |
| --- | --- |
| Export logic + CSV writer | `src/services/export_service.cpp`, `include/services/export_service.h` |
| Export screen (UI) | `src/ui/screens/scr_export.cpp` |
| Footer nav button | `src/ui/components/shell.cpp` (`add_footer`) |
| Screen id | `SCREEN_EXPORT` in `include/ui/screen.h` |
| Registration | `src/ui/ui.cpp` |
| Tests | `test/native/test_export/main.cpp` |

### API (`export_service.h`)

```c
// Days (# session dates), students, and date range for one class.
// Does not disturb any open attendance session.
export_metrics_t export_metrics(const class_rec_t* cls);

// The file path this class exports to ("/csv_export/<code>.csv").
void export_path_for(const class_rec_t* cls, char* out, size_t cap);

// True if an export file for this class already exists (ask before overwrite).
bool export_exists(const class_rec_t* cls);

// Writes MATRICULA,FREQ for every enrolled student, creating the folder and
// overwriting any existing file. Restores any open attendance session.
// Returns { ok, path, size_bytes, message }.
export_result_t export_write_csv(const class_rec_t* cls);
```

`export_service` composes `roster_service` (class/student data) and
`attendance_store` (per-day logs) and writes with the same atomic-friendly SD
pattern as the rest of the project. It is compiled in the native build filter
(`platformio.ini`) and unit-tested against the in-memory SD mock.

### Tests

`test/native/test_export/` covers:

- metrics (students, day count, start/end dates), including the no-attendance case;
- the export path and `export_exists`;
- exact CSV content for a known absence pattern (FREQ `2,1,0`);
- overwrite replaces rather than appends (single header, updated counts);
- an in-progress roll-call session survives an export.

Run just this suite:

```sh
pio test -e native -f "native/test_export"
```

## Status

Build- and native-test-verified only. The CSV/FREQ logic is covered by unit
tests against the SD mock, but the on-device SD write, folder creation, file
size readout, and the screen itself have not been exercised on real hardware.
