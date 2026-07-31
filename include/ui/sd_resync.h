#pragma once

// Drops the UI's cached view of the SD card after the debug web file editor
// changed something under it (see services/file_server.h). Both calls are
// no-ops until file_server_write_count() moves, so they are cheap enough to
// put at the top of an on_show.
//
// The split exists because reloading the roster is NOT safe everywhere:
// roster_service_reload() rewrites the static storage that scr_class holds a
// class_rec_t* into, so a reload under an open class screen can leave it
// pointing at a different class. Screens that hold such a pointer take the
// light call; only screens that re-read the roster from scratch take the full
// one.
//
// LVGL thread only (both do SD I/O through the services).

// Memoised attendance counts only. Safe on any screen.
void ui_sd_resync_light(void);

// The above plus a config + roster reload. Only from screens that hold no
// pointer into roster storage across the call — today the class list and the
// idle gate.
void ui_sd_resync_full(void);
