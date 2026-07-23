#pragma once

// Debug HTTP file manager, served over the soft-AP (see wifi_ap.h). A browser
// can list, download, upload, edit and delete SD-card files. Full access, on
// purpose — this is a debug tool.
//
// The server is synchronous: file_server_handle() must be pumped frequently
// from the LVGL thread, which keeps ALL SD I/O on that one thread (no locking
// against roster/attendance/config). Do not call these from another task.

void file_server_begin(void);   // bind :80 and register routes
void file_server_end(void);     // stop listening
void file_server_handle(void);  // process pending requests; call often
bool file_server_running(void);
