#pragma once

#include <lvgl.h>
#include <stddef.h>

#include "app/roster.h"

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

// How many students in this class have an avatar on the card. One directory
// listing, matched against the roster in RAM — cheap enough for a view rebuild,
// but still SD I/O on the LVGL thread: don't call it per card scan.
int student_photo_count_for_class(const class_rec_t* cls);

// The box every full-size avatar is fitted into (check-in overlay, kiosk
// result). The face-verify modal passes its own, smaller box.
constexpr int AVATAR_MAX_PX = 250;

// Creates an lv_image showing the student's avatar, scaled to fit
// `max_px` x `max_px` with the aspect ratio preserved. Authored avatars are
// smaller than that, so this normally ENLARGES them; anything bigger is shrunk
// to fit. The widget is sized to the drawn pixels, so flex layouts and
// clip_corner see the real footprint.
//
// Returns NULL when the student has no usable photo (missing, unreadable, or
// an unparseable JPEG header) — the caller draws its own placeholder.
lv_obj_t* student_photo_image(lv_obj_t* parent, const char* student_id, int max_px);
