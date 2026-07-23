#include "ui/ui_state.h"

#include "services/config_service.h"
#include "services/roster_service.h"

lv_subject_t ui_subj_wifi_rssi;
lv_subject_t ui_subj_wifi_ap;
lv_subject_t ui_subj_battery_pct;
lv_subject_t ui_subj_battery_mv;
lv_subject_t ui_subj_config_status;
lv_subject_t ui_subj_roster_status;
lv_subject_t ui_subj_user;

// Default top-bar label when nobody is logged in.
static constexpr const char* DEVICE_NAME = "Attendance";

// String subjects need a live buffer plus a previous-value buffer for change
// detection.
static char s_user_buf[64];
static char s_user_prev[64];

void ui_state_init(void) {
    lv_subject_init_int(&ui_subj_wifi_rssi, UI_WIFI_DISCONNECTED);
    lv_subject_init_int(&ui_subj_wifi_ap, 0);
    lv_subject_init_int(&ui_subj_battery_pct, 100);
    lv_subject_init_int(&ui_subj_battery_mv, 4000);  // plausible full charge until first sample
    lv_subject_init_int(&ui_subj_config_status, CONFIG_NO_SD);
    lv_subject_init_int(&ui_subj_roster_status, ROSTER_NO_SD);
    lv_subject_init_string(&ui_subj_user, s_user_buf, s_user_prev, sizeof(s_user_buf),
                           DEVICE_NAME);
}

void ui_state_set_net(bool connected, int8_t rssi) {
    lv_subject_set_int(&ui_subj_wifi_rssi, connected ? rssi : UI_WIFI_DISCONNECTED);
}

void ui_state_set_wifi_ap(bool active) {
    lv_subject_set_int(&ui_subj_wifi_ap, active ? 1 : 0);
}

void ui_state_set_power(uint8_t pct, uint16_t mv) {
    lv_subject_set_int(&ui_subj_battery_mv, mv);
    lv_subject_set_int(&ui_subj_battery_pct, pct > 100 ? 100 : pct);
}

void ui_state_set_config(uint8_t status) {
    lv_subject_set_int(&ui_subj_config_status, status);
}

void ui_state_set_roster(uint8_t status) {
    lv_subject_set_int(&ui_subj_roster_status, status);
}

void ui_state_set_user(const char* name) {
    lv_subject_copy_string(&ui_subj_user, (name && name[0]) ? name : DEVICE_NAME);
}
