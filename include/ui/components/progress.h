#pragma once

#include <lvgl.h>

#include "app/progress.h"

// Full-screen "this is working" overlay for the operations that block the LVGL
// thread — config import/revert, CSV export, and the two debug wipes.
//
// Those run synchronously for seconds to minutes without returning to
// lv_timer_handler(), so nothing repaints on its own. ui_progress_set() forces
// the redraw itself (lv_refr_now), the same trick scr_wifi_editor uses around
// the blocking WiFi start. The screen therefore UPDATES but is not INTERACTIVE:
// touch is not sampled while the caller is blocked, so this deliberately carries
// no buttons — a Cancel here could not be pressed, and cancelling a wipe or an
// apply half-way is exactly what must not happen.
//
// LVGL thread only.

// Shows the overlay with `title` as its heading. Safe to call when one is
// already open (it is reused). Pair with ui_progress_close() on EVERY path.
void ui_progress_open(const char* title);

// Second line, for a caller's own outer loop ("Class 2 of 5") while the service
// drives the detail line underneath. NULL or "" hides it.
void ui_progress_context(const char* line);

// Stage line ("Unpacking 143/612") plus the item being worked on, and the bar.
// `total > 0` makes the bar determinate; 0 hides it and shows text only.
//
// Forces a repaint, throttled internally to REPAINT_MS — except when `stage`
// changes, which always draws. `stage` and `detail` are separate for exactly
// that reason: `detail` changes on every iteration, so throttling on it would
// force a full redraw per file and cost more than the work being reported.
void ui_progress_set(const char* stage, const char* detail, int done, int total);

// Tears the overlay down. A no-op when nothing is open, so it can be called
// unconditionally after an operation and defensively from a screen's on_hide.
//
// Must not be skipped on the failure paths: this is a full-screen CLICKABLE
// object on lv_layer_top(), a GLOBAL layer that survives screen changes, so one
// left behind would swallow every touch on every screen until reboot.
void ui_progress_close(void);

// progress_cb_t adapter — pass this straight to any producer:
//
//   ui_progress_open("Importing configuration");
//   import_result_t r = import_service_run(path, ui_progress_cb, nullptr);
//   ui_progress_close();
//
// `ctx` is unused (the overlay is a singleton); it exists to match the callback
// signature.
void ui_progress_cb(const progress_t* p, void* ctx);
