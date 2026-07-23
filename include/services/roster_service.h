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
//         "schedule": "...", "teacher_email": "...", "color": "272766",
//         "roster": [ { "id": "2023-0142" }, ... ] }
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
// class_code.
roster_result_t roster_enroll_new(const char* class_code, const char* id, const char* name,
                                  const char* uid);

// DEBUG: unbinds every student's RFID card — clears rfid_uid in RAM and rewrites
// students.json with all bindings null. Returns false on write failure. Class
// rosters are untouched (students stay enrolled, just without a card).
bool roster_clear_all_uids(void);

// True if the class should be listed for this teacher. An empty/NULL email
// (password "Staff" login) sees every class.
static inline bool roster_class_matches_teacher(const class_rec_t* c, const char* email) {
    if (!email || email[0] == '\0') return true;
    for (int i = 0; c->teacher_email[i] || email[i]; i++) {
        char a = c->teacher_email[i], b = email[i];
        if (a >= 'A' && a <= 'Z') a += 32;
        if (b >= 'A' && b <= 'Z') b += 32;
        if (a != b) return false;
    }
    return true;
}
