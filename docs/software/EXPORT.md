# CSV Export

Exports per-class attendance to CSV files on the SD card, one file per class.
The feature is reachable from the **Export** button in the bottom navigation bar.

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

Export is a multi-select. Each class assigned to the **logged-in professor** is a
card whose class name doubles as a checkbox label — tapping the row toggles it —
showing:

- **Number of students** enrolled in the class.
- **Number of days** of recorded sessions.
- **Start → end dates**, or "No sessions recorded yet" when there is no
  attendance.
- A hint when a previous export file already exists.

A **Select all / Clear all** shortcut sits above the list.

### Export flow

1. Tick one or more classes. The button at the bottom reads **"Export Selected
   (N)"** and is disabled while nothing is ticked.
2. Tap it. If any of the selected classes already has an export file, a single
   **overwrite-all** confirmation appears — one dialog for the batch, not one per
   file. Overwriting truncates and rewrites; files are never appended to.
3. Each card's status line then shows the saved path and size, e.g.
   `✓ /csv_export/CS101-M1.csv (1.37 kB)`, or the failure reason.

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

Shipped and verified on device. The CSV/FREQ logic is covered by native tests
against the SD mock.
