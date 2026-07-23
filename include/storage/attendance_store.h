#pragma once

#include <stddef.h>

// Per-class, per-day attendance in append-only JSONL files:
//
//   /classes/<dir>/attendance/YYYY-MM-DD.jsonl
//   {"id":"2023-0142","present":true}
//   {"id":"2023-0187","present":true}
//   {"id":"2023-0142","present":false}   <- last line for an id wins
//
// Append-only is deliberate: on a battery-less device that can be switched
// off mid-class, a failed append loses at most the last tap, never the file.
// The "current" presence is the fold of the file (last writer per id wins).
//
// One session is open at a time (all functions touch that session). LVGL
// thread only.

// Opens the session for class_dir + date, folding any existing file into
// memory (a missing file is a fresh, empty session). Replaces any open
// session. Returns false only on a read error.
bool attendance_open(const char* class_dir, const char* date);

bool attendance_is_open(void);
const char* attendance_dir(void);   // "" when no session is open
const char* attendance_date(void);  // ""

bool attendance_is_present(const char* student_id);
int attendance_present_count(void);

// Records a presence change: appends one line and updates memory. Returns
// false on a write error (memory is still updated so the UI stays in sync).
bool attendance_set(const char* student_id, bool present);

// Ends the open session (clears memory; the log is already on disk).
void attendance_close(void);

// Past session dates for a class, newest first. Returns the count, filling up
// to `max` entries of `dates` (each buffer holds "YYYY-MM-DD" + NUL).
int attendance_list_dates(const char* class_dir, char dates[][12], int max);

// Present count for a specific past date without disturbing the open session
// (for the history summary). 0 if the file is missing.
int attendance_present_for(const char* class_dir, const char* date);

// DEBUG: deletes every session log for a class (all YYYY-MM-DD.jsonl files) and
// closes any open session. Returns the number of files removed.
int attendance_clear(const char* class_dir);
