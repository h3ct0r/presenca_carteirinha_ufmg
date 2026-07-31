#pragma once

#include <stdint.h>

// Debug HTTP file manager, served over the soft-AP (see wifi_ap.h). A browser
// can list, download, upload (button or drag-and-drop into the open folder),
// edit, rename and delete SD-card entries —
// including deleting a folder with everything under it (`recursive=1`, which
// the UI sends only after a confirm). Full access, on purpose — this is a
// debug tool. The tree walking lives in storage/sd_tree.h.
//
// The server is synchronous, so it runs on its own FreeRTOS task ("fileserv")
// instead of being pumped from the LVGL thread — a large upload or download
// would otherwise freeze the UI for the whole transfer. Its SD access is
// therefore concurrent with the rest of the firmware, and relies on the same
// FatFs volume lock (FF_FS_REENTRANT) that photo_store's writer task and the
// roster/config retry tasks already do: no corruption, but a file edited over
// the web while a screen reads it can still be seen half-written. Acceptable
// for a debug tool the professor starts deliberately.
//
// begin()/end() are for the LVGL thread only; the task itself is internal.

void file_server_begin(void);  // bind :80, register routes, start the task
void file_server_end(void);    // ask the task to stop and wait briefly for it
bool file_server_running(void);

// Counts every successful change the browser made to the card (save, delete,
// rename, upload). It only ever means "something changed since you last
// looked" — compare it against a stored copy, never for a magnitude. The UI
// uses it to drop its cached view of the card; see ui/sd_resync.h.
uint32_t file_server_write_count(void);
