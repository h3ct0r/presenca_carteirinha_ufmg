#pragma once

// Progress reporting for the long operations that block the LVGL thread —
// config import/revert, CSV export, and the two debug wipes. Each of those runs
// for seconds to minutes without yielding, so without this the screen sits on
// its last frame and reads as a crash.
//
// One shared shape rather than an enum per service: the alternative needed a
// phase->string table in the UI for every operation, and services here already
// own the user-facing wording (import_result_t::message and
// roster_result_t::message are shown verbatim on screen).
//
// The callback runs on the CALLER's thread — which for every current producer is
// the LVGL thread — so the UI adapter may touch LVGL directly. That is the whole
// point: it repaints mid-operation. A service must never call it from a task of
// its own.
//
// Pure types, no hardware: app/ so services/ and storage/ can both use it and
// the native tests compile it.

typedef struct {
    // What is happening, in the user's words: "Unpacking", "Deleting". Never
    // NULL — the UI shows it as the headline and uses a change of stage to force
    // a repaint even when the throttle would otherwise skip one.
    const char* stage;
    // The item being worked on (a file or class), or NULL when there isn't one.
    const char* detail;
    int done;   // items finished in this stage
    int total;  // 0 = indeterminate (the count is not known up front)
} progress_t;

// Always optional: every producer takes it as a trailing parameter and a NULL
// callback means "report nothing", which is what the native tests and the
// non-UI callers (e.g. the file server's own task) pass.
typedef void (*progress_cb_t)(const progress_t* p, void* ctx);
