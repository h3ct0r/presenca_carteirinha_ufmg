#include "ui/components/status_bar.h"

#include <stdio.h>

#include "ui/theme/theme.h"
#include "ui/ui_state.h"

static constexpr uint32_t COL_BG = THEME_DARK_BG_DEEP;
static constexpr uint32_t COL_TEXT = THEME_DARK_TEXT;
static constexpr uint32_t COL_MUTED = THEME_MUTED;
static constexpr uint32_t COL_WARN = THEME_DARK_WARN;
static constexpr uint32_t COL_OK = THEME_DARK_OK;  // AP-active WiFi icon

static lv_obj_t* s_title_label = nullptr;
static lv_obj_t* s_wifi_label = nullptr;
static lv_obj_t* s_batt_label = nullptr;

static void user_changed_cb(lv_observer_t*, lv_subject_t* subject) {
    lv_label_set_text(s_title_label, lv_subject_get_string(subject));
}

// Bound to both the pct and mv subjects (they update together); reads both so
// the label carries "<icon> <pct>% (<v>.<vv> V)".
static void battery_changed_cb(lv_observer_t*, lv_subject_t*) {
    int pct = lv_subject_get_int(&ui_subj_battery_pct);
    int mv = lv_subject_get_int(&ui_subj_battery_mv);

    const char* icon = (pct > 85)   ? LV_SYMBOL_BATTERY_FULL
                       : (pct > 60) ? LV_SYMBOL_BATTERY_3
                       : (pct > 35) ? LV_SYMBOL_BATTERY_2
                       : (pct > 15) ? LV_SYMBOL_BATTERY_1
                                    : LV_SYMBOL_BATTERY_EMPTY;

    char text[40];
    snprintf(text, sizeof(text), "%s %d%% (%d.%02d V)", icon, pct, mv / 1000, (mv % 1000) / 10);
    lv_label_set_text(s_batt_label, text);

    bool low = pct <= 15;
    lv_obj_set_style_text_color(s_batt_label, lv_color_hex(low ? COL_WARN : COL_TEXT), 0);
}

// Bound to both the rssi and ap subjects. The debug soft-AP wins (green);
// otherwise white when connected as a client, muted gray when off.
static void wifi_changed_cb(lv_observer_t*, lv_subject_t*) {
    bool ap = lv_subject_get_int(&ui_subj_wifi_ap) != 0;
    bool connected = lv_subject_get_int(&ui_subj_wifi_rssi) != UI_WIFI_DISCONNECTED;
    uint32_t col = ap ? COL_OK : (connected ? COL_TEXT : COL_MUTED);
    lv_obj_set_style_text_color(s_wifi_label, lv_color_hex(col), 0);
}

void status_bar_create(void) {
    lv_obj_t* bar = lv_obj_create(lv_layer_top());
    lv_obj_set_size(bar, LV_PCT(100), STATUS_BAR_HEIGHT);
    lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_pad_hor(bar, 14, 0);
    lv_obj_set_style_pad_ver(bar, 0, 0);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(bar, 14, 0);

    s_title_label = lv_label_create(bar);
    lv_obj_set_style_text_color(s_title_label, lv_color_hex(COL_TEXT), 0);
    lv_label_set_long_mode(s_title_label, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_title_label, 240);  // truncate long professor names

    // Spacer pushes the indicators to the right edge.
    lv_obj_t* spacer = lv_obj_create(bar);
    lv_obj_set_height(spacer, 1);
    lv_obj_set_flex_grow(spacer, 1);
    lv_obj_set_style_bg_opa(spacer, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(spacer, 0, 0);

    s_wifi_label = lv_label_create(bar);
    lv_label_set_text(s_wifi_label, LV_SYMBOL_WIFI);

    s_batt_label = lv_label_create(bar);

    // Observers fire once on registration, so this also paints the initial
    // (boot-default) state.
    lv_subject_add_observer(&ui_subj_user, user_changed_cb, nullptr);
    lv_subject_add_observer(&ui_subj_wifi_rssi, wifi_changed_cb, nullptr);
    lv_subject_add_observer(&ui_subj_wifi_ap, wifi_changed_cb, nullptr);
    lv_subject_add_observer(&ui_subj_battery_pct, battery_changed_cb, nullptr);
    lv_subject_add_observer(&ui_subj_battery_mv, battery_changed_cb, nullptr);
}
