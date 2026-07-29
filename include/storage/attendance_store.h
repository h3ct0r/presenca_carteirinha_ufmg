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
// In timed mode a student taps twice: once on arrival, once to confirm after
// they have been in class long enough. The confirming tap only counts once
// (now - arrive) >= threshold; a tap before that is rejected without changing
// anything, so the student can simply come back later — the arrival still
// stands and the rejected tap reports the minutes still to wait. Once the
// presence is registered, further taps are ignored (ATT_ALREADY_PRESENT).
//
// Elapsed time comes from a monotonic clock (esp_timer_get_time), so it works
// without an RTC. The "tapped in, not confirmed" state lives in RAM only — a
// reboot voids it. Registered results are written to the same JSONL with an
// extra "min" field:
//   {"id":"2023-0142","present":true,"min":52}

typedef enum {
    ATT_ABSENT = 0,       // not tapped
    ATT_IN_PROGRESS,      // tapped in, waiting out the threshold
    ATT_PRESENT,          // this tap registered the presence
    ATT_TOO_EARLY,        // tapped before the threshold: nothing recorded, still waiting
    ATT_ALREADY_PRESENT,  // presence was already registered; this tap is ignored
} att_status_t;

typedef struct {
    att_status_t status;
    int minutes;    // elapsed so far (IN_PROGRESS/TOO_EARLY) or measured (PRESENT/ALREADY_PRESENT)
    int remaining;  // whole minutes left before a tap can register (IN_PROGRESS/TOO_EARLY; else 0)
} att_state_t;

// Handles a timed tap for a student in the open session. The first tap records
// the arrival (RAM only). A later tap registers the presence (writes
// present+min) once the gap is >= threshold_min; before that it changes nothing
// and returns ATT_TOO_EARLY with `remaining` set, and once registered it
// returns ATT_ALREADY_PRESENT. `now_us` is a monotonic timestamp
// (microseconds). Returns the resulting state.
att_state_t attendance_tap(const char* student_id, long long now_us, int threshold_min);

// The student's current timed state in the open session (for the roll call).
// A query, never a tap: it reports ATT_IN_PROGRESS (with the minutes elapsed so
// far and still to wait, per threshold_min) or ATT_PRESENT, never TOO_EARLY /
// ALREADY_PRESENT.
att_state_t attendance_tap_state(const char* student_id, long long now_us, int threshold_min);

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
