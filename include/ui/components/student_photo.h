#pragma once

#include <stddef.h>

// Fills `out` with the LVGL image-source path for a student's avatar
// ("S:/students/photos/<id>.jpg") and returns true when that file exists on the
// SD card; returns false (and empties `out`) otherwise, so the caller shows a
// fallback. The avatars are the authored, id-keyed photos (see
// docs/software/STUDENT_PHOTOS.md), distinct from the device's own /photos
// check-in snapshots. Rendering relies on the SD 'S' fs driver
// (lvgl_fs_sd_init) and the TJPGD decoder (LV_USE_TJPGD).
bool student_photo_src(const char* student_id, char* out, size_t cap);

// True if a student's avatar file exists on the SD card. Cheaper than
// student_photo_src() when only a count is needed (no path string built for the
// caller). Does SD I/O — avoid calling it in a tight per-scan loop; count once.
bool student_photo_exists(const char* student_id);
