#pragma once

#include <lvgl.h>

// Observable UI state, LVGL 9 subjects. Widgets (status bar, config screen)
// bind observers to these once at creation and are then updated
// automatically whenever a setter runs — no manual refresh calls, no widget
// knowing where the data comes from.
//
// Setters are called from the event drain path, so everything here is LVGL
// thread only.

// int subject: RSSI in dBm while connected, UI_WIFI_DISCONNECTED otherwise.
extern lv_subject_t ui_subj_wifi_rssi;
constexpr int UI_WIFI_DISCONNECTED = 127;

// int subject: 1 while the debug soft-AP is broadcasting (turns the top-bar
// WiFi icon green), 0 otherwise.
extern lv_subject_t ui_subj_wifi_ap;

// int subject: battery percentage 0..100.
extern lv_subject_t ui_subj_battery_pct;

// int subject: battery voltage in millivolts, shown beside the percentage.
extern lv_subject_t ui_subj_battery_mv;

// int subject: config_status_t value (CONFIG_OK etc.). Screens observe this
// to prompt for a valid SD card / config.json.
extern lv_subject_t ui_subj_config_status;

// int subject: roster_status_t value (ROSTER_OK etc.) — validity of the
// student/class data layout on the SD card.
extern lv_subject_t ui_subj_roster_status;

// string subject: the logged-in professor's name shown in the top bar, or the
// device name when nobody is logged in.
extern lv_subject_t ui_subj_user;

// Initializes all subjects to boot defaults (offline, battery unknown-full,
// config not ready). Call before any component binds to them.
void ui_state_init(void);

void ui_state_set_net(bool connected, int8_t rssi);
void ui_state_set_wifi_ap(bool active);
void ui_state_set_power(uint8_t pct, uint16_t mv);
void ui_state_set_config(uint8_t status);
void ui_state_set_roster(uint8_t status);

// Sets the top-bar user label. NULL or empty resets it to the device name.
void ui_state_set_user(const char* name);
