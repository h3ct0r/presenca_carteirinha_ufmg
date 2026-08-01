#pragma once

#include <stddef.h>
#include <stdint.h>

#include "app/progress.h"
#include "app/roster.h"

// Exports per-class attendance to CSV files on the SD card, under /csv_export/.
// Each file is named <class code>.csv and holds one row per enrolled student:
//
//   MATRICULA,FREQ
//   202500001,1
//   202500002,2
//
// MATRICULA is the student's university id; FREQ is how many recorded session
// days that student was absent (missing) across the class's whole history.
//
// Pure-ish logic over roster_service + attendance_store + SD, so it is
// unit-tested natively. LVGL thread only (does SD I/O; call off the render
// hot path).

#define EXPORT_DIR "/csv_export"

// Summary metrics shown on the export screen for one class.
typedef struct {
    int student_count;    // enrolled students
    int day_count;        // number of recorded session dates
    char start_date[12];  // earliest session date, "" if none
    char end_date[12];    // latest session date, "" if none
} export_metrics_t;

// Outcome of a CSV export.
typedef struct {
    bool ok;
    char path[80];        // "/csv_export/<code>.csv" on success
    uint32_t size_bytes;  // written file size
    char message[80];     // failure reason when !ok
} export_result_t;

// Days (# session dates) and students for a class, plus the date range. Does
// not disturb any currently-open attendance session.
export_metrics_t export_metrics(const class_rec_t* cls);

// Fills `out` with the file path this class exports to ("/csv_export/<code>.csv").
void export_path_for(const class_rec_t* cls, char* out, size_t cap);

// True if an export file for this class already exists (ask before overwrite).
bool export_exists(const class_rec_t* cls);

// Writes MATRICULA,FREQ for every enrolled student to /csv_export/<code>.csv,
// creating the folder and overwriting any existing file. Restores any open
// attendance session afterward. Returns {ok,path,size_bytes,message}.
//
// `cb` (optional) reports one item per session date folded — the loop that makes
// this slow, since every date is a whole-file read. A class with a full year of
// sessions is 366 of them, and the export screen runs this once per ticked class.
export_result_t export_write_csv(const class_rec_t* cls, progress_cb_t cb, void* ctx);
