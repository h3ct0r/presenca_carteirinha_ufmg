#pragma once

#include <stddef.h>
#include <stdint.h>

#include "app/roster.h"

// Loads and validates the student/class data layout on the SD card:
//
//   /students/students.json   global registry:
//       { "version": 1, "students": [
//           { "id": "2023-0142", "name": "Maria Santos",
//             "rfid_uid": "04:A3:1B:2C" | null } ] }
//   /students/photos/<id>.jpg avatars, named by university ID
//   /classes/<code>/class.json  metadata + roster (university IDs):
//       { "version": 1, "code": "CS101-M1", "name": "...",
//         "schedule": "...", "teacher_emails": ["a@x.edu", "b@x.edu"],
//         "color": "272766",
//         "roster": [ { "id": "2023-0142" }, ... ] }
//       (a legacy scalar "teacher_email" is still read when the array is absent)
//   /classes/<code>/attendance/YYYY-MM-DD.jsonl  per-session logs (later)
//
// Validation is strict and the failure reason is kept as a human-readable
// message (roster_get_error) naming the file and problem, so the idle screen
// can tell the user exactly what to fix. A missing /classes directory is
// allowed (zero classes); a malformed file anywhere is not.

typedef enum : uint8_t {
    ROSTER_OK = 0,
    ROSTER_NO_SD,             // mount failed (no card / not FAT32)
    ROSTER_NO_STUDENTS_FILE,  // /students/students.json missing
    ROSTER_BAD_STUDENTS,      // students.json unparseable or invalid
    ROSTER_BAD_CLASS,         // some class.json missing/unparseable/invalid
} roster_status_t;

// Synchronous first load (call in setup(), after event_bus_init() and before
// ui_init()), then a background retry every few seconds until the data is
// valid, so fixing the card updates the UI. Publishes APP_EVENT_ROSTER_STATE
// on every state change.
bool roster_service_start(void);

// Re-reads the student/class layout and republishes the status. For the
// importer to call after applying a new roster. LVGL/import thread.
void roster_service_reload(void);

roster_status_t roster_get_status(void);

// Human-readable reason for the current failure ("classes/CS101-M1: unknown
// student 2023-9999 in roster"). Empty string when status is ROSTER_OK.
void roster_get_error(char* out, size_t cap);

// Read accessors. Pointers reference the service's static storage: valid
// only while the status is ROSTER_OK, and stable from then on (the loader
// task exits once the data is valid). LVGL-thread reads are safe under that
// rule. NULL/0 when out of range or data not loaded.
int roster_student_count(void);
int roster_class_count(void);
const class_rec_t* roster_class_at(int idx);
const student_t* roster_student_at(int idx);

// Index of the class with this code, or -1.
int roster_class_index(const char* code);

// Validates a staged roster tree rooted at `root` (e.g. "/import_staging") with
// the exact live rules, WITHOUT changing what the service publishes — used by
// the config importer to check a tar before applying it. Reads
// "<root>/students/students.json" and "<root>/classes/<CODE>/class.json".
// Returns true when valid; on failure fills `msg` with the reason (empty on
// success). Internally borrows the shared arrays as scratch and restores the
// live tree before returning, all under the roster lock; call on the import
// (LVGL) thread (SD I/O).
bool roster_validate_tree(const char* root, char* msg, size_t cap);

// True if `uid` (in any format — canonicalized internally) is currently bound
// to a student. On a match, copies that student's name into name_out (may be
// NULL, cap the buffer size). Returns false when the roster isn't loaded, so
// the caller can't rely on it as proof of "no student" when the SD data is
// broken. Mirrors the professor-collision guard the enroll path applies to
// students; used by config_set_rfid to keep a professor from taking a card
// that already belongs to a student. LVGL-thread reads are safe.
bool roster_uid_belongs_to_student(const char* uid, char* name_out, size_t cap);

// --- Enrollment writes (SD card) -------------------------------------------
//
// Bind a student's RFID card and make sure they are in a class's roster,
// persisting to /students/students.json and /classes/<dir>/class.json. Both
// update the in-RAM data on success and refuse a UID already assigned to a
// different student. Call from the LVGL thread (the enroll flow).

typedef struct {
    bool ok;
    char message[96];  // human-readable result, for the on-screen toast
} roster_result_t;

// Binds uid to the existing registry student at student_idx and enrolls them
// in class_code.
roster_result_t roster_enroll_existing(const char* class_code, int student_idx,
                                       const char* uid);

// Adds a brand-new student (id/name/uid) to the registry and enrolls them in
// class_code, tagging the class-roster entry with `turma` (may be NULL/empty;
// max 15 chars — see CONFIG_IMPORT.md §3.3). The turma lives only on the
// class.json roster entry, not the global registry.
roster_result_t roster_enroll_new(const char* class_code, const char* id, const char* name,
                                  const char* uid, const char* turma);

// Whether photo check-in is enabled for this class (class.json "capture_photos";
// a per-class option, there is no device-wide flag). False for a NULL class.
bool class_capture_enabled(const class_rec_t* cls);

// Updates a class's editable metadata (name, schedule, color), its photo check-in
// (`capture` + `face_verify_seconds`), and its timed-attendance settings (`timed`
// + `min_attendance_min`), rewriting /classes/<dir>/class.json atomically and
// updating the in-RAM record. The roster (and each entry's turma) is preserved.
// Returns {ok,message}. Call on the LVGL thread (SD I/O).
roster_result_t roster_class_update_settings(const char* class_code, const char* name,
                                             const char* schedule, uint32_t color, bool capture,
                                             int face_verify_seconds, bool timed,
                                             int min_attendance_min);

// DEBUG: unbinds every student's RFID card — clears rfid_uid in RAM and rewrites
// students.json with all bindings null. Class rosters are untouched (students
// stay enrolled, just without a card). On failure the result carries a specific
// reason (roster not loaded / read failed / write failed) for the toast, and the
// underlying cause is logged.
roster_result_t roster_clear_all_uids(void);

// How many class folders were skipped by the last live load because they could
// not be parsed (missing/invalid class.json, roster referencing students that no
// longer exist — typically a leftover folder from a previous config, since an
// import is an overlay and never deletes classes). Such a class is skipped
// instead of failing the whole load, so the rest of the classes stay usable;
// these two let the UI say so rather than silently showing fewer classes.
// Always 0 after a successful load with no problems.
int roster_skipped_class_count(void);

// The reason the FIRST skipped class was skipped (empty when none were).
void roster_get_skip_reason(char* out, size_t cap);

// Case-insensitive ASCII compare for the email keys that link classes to
// config.json teachers (emails are stored exactly as authored).
static inline bool roster_email_equal(const char* a, const char* b) {
    for (int i = 0;; i++) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return false;
        if (x == '\0') return true;
    }
}

// True if the class should be listed for this teacher. An empty/NULL email
// (password "Staff" login) sees every class. A class may be co-taught, so this
// matches if ANY of its professors is this one.
static inline bool roster_class_matches_teacher(const class_rec_t* c, const char* email) {
    if (!email || email[0] == '\0') return true;
    for (int i = 0; i < c->teacher_count; i++) {
        if (roster_email_equal(c->teacher_emails[i], email)) return true;
    }
    return false;
}
