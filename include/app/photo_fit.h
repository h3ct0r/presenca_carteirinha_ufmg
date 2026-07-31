#pragma once

// Aspect-preserving "fit an image inside a box" arithmetic for the avatar
// widgets (ui/components/student_photo). Pure integer math, no LVGL and no
// hardware, so it lives in app/ and is covered by the native tests.

// Fixed-point scale unit: 256 == 1:1, 128 == half size, 512 == double. Matches
// LVGL's LV_SCALE_NONE, which student_photo.cpp static_asserts against — this
// header stays free of lvgl.h so the math remains testable on the host.
constexpr int PHOTO_SCALE_UNIT = 256;

// Scale that fits src_w x src_h inside max_px x max_px with the aspect ratio
// preserved. ENLARGES when the source is smaller than the box (a 100x100 avatar
// in a 250 box returns 640), so the result is a fit, not just a cap.
// Non-positive inputs return PHOTO_SCALE_UNIT (draw it 1:1 rather than divide
// by zero) — a JPEG whose header failed to parse reports 0.
int photo_fit_scale(int src_w, int src_h, int max_px);

// The on-screen size photo_fit_scale() produces. Neither axis ever exceeds
// max_px, and a visible source never rounds away to a zero-sized widget (each
// axis is at least 1). Writes 0x0 for non-positive inputs. Either out pointer
// may be NULL.
void photo_fit_size(int src_w, int src_h, int max_px, int* out_w, int* out_h);
