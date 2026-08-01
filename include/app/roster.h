#pragma once

#include <stdint.h>

// Domain types for the SD-card student/class data (see roster_service.h for
// the on-card layout). Students live in one global registry keyed by
// university ID; classes reference them by registry index, so a student
// enrolled in several classes has exactly one record — and one RFID binding.

constexpr int ROSTER_MAX_STUDENTS = 600;   // global registry cap (see the RAM note below)
constexpr int ROSTER_MAX_CLASSES = 12;
// Per-class roster cap. Raising this is not free: it widens class_rec_t (which
// is held ROSTER_MAX_CLASSES times) AND attendance_store's present/tap-in sets,
// all in internal RAM. See the RAM note below for the measured cost.
constexpr int ROSTER_MAX_CLASS_STUDENTS = 200;
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
// NOTE: these caps are paid for in INTERNAL RAM, the scarce pool
// (docs/software/ARCHITECTURE.md §Memory budget). Measured on the v0.2.0 build
// with `riscv32-esp-elf-nm --size-sort -S`:
//
//   s_students[600]  55,200 B   600 * sizeof(student_t) (92 B)
//   s_classes[12]    51,216 B   12 * sizeof(class_rec_t) (4,268 B)
//   attendance_store 10,400 B   present + tap-in sets, sized by the class cap
//
// Raising ROSTER_MAX_CLASS_STUDENTS from 100 to 200 cost 26,808 B of that:
// +21,600 in s_classes (roster_turma is ~75% of it) and +5,208 in
// attendance_store. Total static internal is 160 KB, up from 134 KB.
//
// There is headroom, but this is the budget the face-detection model check
// (128 KB free / 64 KB largest block) and every sdmmc DMA transfer draw from.
// Before raising these again, either measure free internal RAM on device or take
// the lever BACKLOG.md names: move s_students/s_classes to PSRAM at
// roster_service_start().

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
