#pragma once

#include <stddef.h>

// Per-student check-in photos, saved when face-verified capture is on. Grouped
// by student id so a reviewer can open one folder and confirm the same person
// tapped over time (see docs/software/FACE_CHECKIN.md):
//
//   /students/checkins/<student_id>/<YYYY-MM-DD>_<CLASS_CODE>_<NN>.jpg
//
// Distinct from the reference avatar (/students/photos/<id>.jpg) and the
// camera's raw snapshots (/photos/**).

// Builds the next unused check-in photo path for (student_id, date, class_code)
// into `out`, creating /students/checkins/<id>/ if needed. `NN` is the next
// 2-digit index after the highest existing file with that date+class prefix (no
// RTC, so the counter disambiguates same-day taps). Returns true on success.
// SD I/O; LVGL/camera thread.
bool checkin_store_next_path(const char* student_id, const char* date, const char* class_code,
                            char* out, size_t cap);
