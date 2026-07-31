#pragma once

#include <stddef.h>

// Debug WiFi soft-AP for the on-device file editor. The ESP32-P4 has no native
// WiFi, so this brings up an access point on the ESP32-C6 companion (via
// esp_wifi_remote). Credentials are generated once per boot:
//   SSID     = "CARTEIRINHA-XXXX" (random hex suffix)
//   password = 8 random decimal digits (WPA2 minimum length)
//
// LVGL thread only.

// Copies the current credentials (generated lazily on first call). `ssid` needs
// >= 33 bytes, `pass` >= 16.
void wifi_ap_credentials(char* ssid, size_t ssid_cap, char* pass, size_t pass_cap);

// Starts the soft-AP with the generated credentials. Returns true on success.
bool wifi_ap_start(void);

// Tears the soft-AP down. Only the visible network goes away: the WiFi stack
// and the P4<->C6 link stay initialized on purpose (see the .cpp), so the
// internal RAM they hold is NOT returned. Nothing reclaims it before a reboot.
void wifi_ap_stop(void);

bool wifi_ap_is_running(void);

// The AP gateway IP as a string ("192.168.4.1"), or "" when not running.
void wifi_ap_ip(char* out, size_t cap);
