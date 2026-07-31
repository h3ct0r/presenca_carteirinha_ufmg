#pragma once

#include <stdint.h>

// Domain types for the SD-card student/class data (see roster_service.h for
// the on-card layout). Students live in one global registry keyed by
// university ID; classes reference them by registry index, so a student
// enrolled in several classes has exactly one record — and one RFID binding.

constexpr int ROSTER_MAX_STUDENTS = 600;   // global registry cap (see the RAM note below)
constexpr int ROSTER_MAX_CLASSES = 12;
constexpr int ROSTER_MAX_CLASS_STUDENTS = 100;
// A class can be co-taught. Mirrors CONFIG_MAX_TEACHERS (config_service.h) —
// a class can't reference more professors than the device can hold. The
// config-builder's schema-sync test asserts the two stay equal.
constexpr int ROSTER_MAX_CLASS_TEACHERS = 8;

// Face-verify countdown bounds (seconds) for a class's photo check-in.
constexpr int FACE_VERIFY_SECONDS_MIN = 3;
constexpr int FACE_VERIFY_SECONDS_MAX = 60;
constexpr int FACE_VERIFY_SECONDS_DEFAULT = 15;
// Timed attendance: minutes a student must wait before the confirming tap
// counts. class.json's optional "min_attendance_min".
constexpr int MIN_ATTENDANCE_MIN_DEFAULT = 45;
// NOTE: s_students[ROSTER_MAX_STUDENTS] is a static array — at 600 that is
// 600 * sizeof(student_t) (~92 B) ≈ 54 KB of internal RAM (~+27 KB vs 300).
// Fine on the P4's SRAM budget, but re-check free heap on device.

typedef struct {
    char id[20];        // university ID, the stable key
    char name[48];
    char rfid_uid[24];  // empty = card not matched yet (bound on first tap)
} student_t;

typedef struct {
    char code[24];           // e.g. "CS101-M1"
    char dir[24];            // /classes/<dir>/ folder name (usually == code)
    char name[48];
    char schedule[40];
    // Professors who teach this class — links to config.json's teachers. A class
    // may be co-taught, so it shows for ANY of these (roster_class_matches_teacher).
    // From class.json's "teacher_emails" array; a legacy scalar "teacher_email"
    // is still accepted and lands here as a single entry.
    char teacher_emails[ROSTER_MAX_CLASS_TEACHERS][64];
    int8_t teacher_count;
    uint32_t color;
    // Photo check-in for this class (a per-class option; there is no device-wide
    // flag). When on, kiosk check-ins face-verify within face_verify_seconds and
    // save a photo. From class.json's optional "capture_photos" bool (default
    // false) + "face_verify_seconds" int (default 15, clamped).
    bool capture_photos;
    int16_t face_verify_seconds;
    // Timed (double-tap) attendance: students tap on arrival, then tap again to
    // register once >= min_attendance_min have passed (an earlier tap is
    // rejected, not recorded). From class.json's optional "timed_attendance"
    // bool + "min_attendance_min" int (default 45).
    bool timed_attendance;
    int16_t min_attendance_min;
    // Per-student class group ("turma") lives on each class.json roster entry
    // ({"id","turma"}). Parallel to roster[]: roster_turma[i] is the group of the
    // student at roster[i] IN THIS CLASS (empty when untagged). Attendance is
    // still by student index; turma is display/statistics only.
    int16_t roster[ROSTER_MAX_CLASS_STUDENTS];   // indexes into the registry
    char roster_turma[ROSTER_MAX_CLASS_STUDENTS][16];
    int roster_count;
} class_rec_t;
