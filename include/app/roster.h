#pragma once

#include <stdint.h>

// Domain types for the SD-card student/class data (see roster_service.h for
// the on-card layout). Students live in one global registry keyed by
// university ID; classes reference them by registry index, so a student
// enrolled in several classes has exactly one record — and one RFID binding.

constexpr int ROSTER_MAX_STUDENTS = 600;   // global registry cap (see the RAM note below)
constexpr int ROSTER_MAX_CLASSES = 12;
constexpr int ROSTER_MAX_CLASS_STUDENTS = 100;
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
    char teacher_email[64];  // links to config.json's teachers
    uint32_t color;
    // Per-student class group ("turma") lives on each class.json roster entry
    // ({"id","turma"}). Parallel to roster[]: roster_turma[i] is the group of the
    // student at roster[i] IN THIS CLASS (empty when untagged). Attendance is
    // still by student index; turma is display/statistics only.
    int16_t roster[ROSTER_MAX_CLASS_STUDENTS];   // indexes into the registry
    char roster_turma[ROSTER_MAX_CLASS_STUDENTS][16];
    int roster_count;
} class_rec_t;
