#pragma once

#include "ui/screen.h"

// Debug WiFi File Editor: brings up a soft-AP (SSID/password shown in clear
// text) serving the browser file manager in services/file_server.cpp — list,
// edit, upload, rename and delete anything on the card. No authentication;
// the footer nav is locked while the AP is up so it cannot be left running.
extern const screen_t scr_wifi_editor;
