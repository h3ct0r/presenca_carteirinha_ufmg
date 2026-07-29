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

// --- Timed (double-tap) attendance ------------------------------------------
//
// In timed mode a student taps twice: once on arrival, once on leaving. They
// only count as present if (leave - arrive) >= threshold. Duration is measured
// from a monotonic clock (esp_timer_get_time), so it works without an RTC. The
// in-progress "tapped in, not out" state lives in RAM only — a reboot voids it.
// Finalized results are written to the same JSONL with an extra "min" field:
//   {"id":"2023-0142","present":true,"min":52}

typedef enum {
    ATT_ABSENT = 0,    // not tapped (or tapped out too early)
    ATT_IN_PROGRESS,   // tapped in, waiting for the tap-out
    ATT_PRESENT,       // tapped out with enough minutes
    ATT_LEFT_EARLY,    // tapped out below the threshold (not counted present)
} att_status_t;

typedef struct {
    att_status_t status;
    int minutes;  // elapsed so far (IN_PROGRESS) or measured (PRESENT/LEFT_EARLY)
} att_state_t;

// Handles a timed tap for a student in the open session. First tap records the
// arrival (RAM); the second finalizes: present (writes present+min) when the
// gap is >= threshold_min, else left-early. `now_us` is a monotonic timestamp
// (microseconds). Returns the resulting state.
att_state_t attendance_tap(const char* student_id, long long now_us, int threshold_min);

// The student's current timed state in the open session (for the roll call).
// now_us lets IN_PROGRESS report the minutes elapsed so far.
att_state_t attendance_tap_state(const char* student_id, long long now_us);

// Ends the open session (clears memory; the log is already on disk).
void attendance_close(void);

// Past session dates for a class, newest first. Returns the count, filling up
// to `max` entries of `dates` (each buffer holds "YYYY-MM-DD" + NUL).
int attendance_list_dates(const char* class_dir, char dates[][12], int max);

// Present count for a specific past date without disturbing the open session
// (for the history summary). 0 if the file is missing.
int attendance_present_for(const char* class_dir, const char* date);

// DEBUG: deletes every session log for a class (all YYYY-MM-DD.jsonl files) and
// closes any open session. Returns the number of files removed. When `out_failed`
// is non-NULL it receives the number of files that could NOT be deleted, so a
// partial wipe isn't reported as a clean success (each failure is also logged).
int attendance_clear(const char* class_dir, int* out_failed);
