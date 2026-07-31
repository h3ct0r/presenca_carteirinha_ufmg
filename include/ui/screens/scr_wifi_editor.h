#pragma once

#include "ui/screen.h"

// Debug WiFi File Editor: brings up a soft-AP (SSID/password shown in clear
// text) serving the browser file manager in services/file_server.cpp — list,
// edit, upload, rename and delete anything on the card. No authentication, so
// the footer nav is locked while the AP is up and it cannot be left running by
// accident. A debug switch on the screen lifts that lock and lets the server
// keep serving from any screen (kiosk, roll call); it is off at every boot and
// is not persisted anywhere.
extern const screen_t scr_wifi_editor;

// Stops the AP and the file server, wherever the UI happens to be, and returns
// true if something was actually running (so the caller can say why it went).
// Two callers: the idle gate, so signing out can never leave the
// unauthenticated file manager on air, and the camera screens — the WiFi stack
// and esp-dl compete for the same internal SIMD-capable RAM, and esp-dl faults
// instead of failing when it runs out. LVGL thread.
bool scr_wifi_editor_stop_ap(void);
