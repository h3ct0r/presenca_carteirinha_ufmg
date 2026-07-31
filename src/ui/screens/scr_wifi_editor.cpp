#include "ui/screens/scr_wifi_editor.h"

#include <stdio.h>

#include "services/file_server.h"
#include "services/wifi_ap.h"
#include "ui/components/shell.h"
#include "ui/sd_resync.h"
#include "ui/theme/theme.h"
#include "ui/ui_state.h"

static shell_t s_sh;
static lv_obj_t* s_ssid_val = nullptr;
static lv_obj_t* s_pass_val = nullptr;
static lv_obj_t* s_btn = nullptr;
static lv_obj_t* s_btn_label = nullptr;
static lv_obj_t* s_status = nullptr;

// Debug: keep the AP + file server up while the professor uses other screens.
// Runtime-only and off at every boot — an unauthenticated file manager must
// never come back on its own after a restart.
static bool s_background = false;

static void refresh_state(void);

// Starts/stops the file server to match the AP state. The server runs on its
// own task (see file_server.h), so nothing has to be pumped from here.
static void sync_file_server(bool running) {
    if (running) {
        if (!file_server_running()) file_server_begin();
    } else {
        if (file_server_running()) {
            file_server_end();
            // Whatever the web editor changed is invisible to the caches that
            // hold it. Only the cheap half here — the roster reload belongs to
            // the class list and the idle gate, which hold no pointer into
            // roster storage (see ui/sd_resync.h).
            ui_sd_resync_light();
        }
    }
}

static void background_toggle_cb(lv_event_t* e) {
    s_background = lv_obj_has_state((lv_obj_t*)lv_event_get_target(e), LV_STATE_CHECKED);
    refresh_state();  // unlocks/relocks the footer nav straight away
}

// Reflects the current AP state onto the button (green when live), the status
// line (the IP, big + centered), the top-bar WiFi icon, and the bottom nav
// (locked while the AP is up unless the background switch is on — the top-bar
// WiFi icon is then the only reminder that it is still serving).
static void refresh_state(void) {
    bool running = wifi_ap_is_running();
    if (running) {
        lv_label_set_text(s_btn_label, LV_SYMBOL_CLOSE "  Stop Access Point");
        // Red = the destructive action this button performs (stop the AP), not
        // the current state. The green "live" signal is the status line + the
        // top-bar WiFi icon below.
        lv_obj_set_style_bg_color(s_btn, lv_color_hex(THEME_DANGER), 0);
        char ip[24];
        wifi_ap_ip(ip, sizeof(ip));
        char msg[96];
        snprintf(msg, sizeof(msg), LV_SYMBOL_OK "  http://%s", ip);
        lv_label_set_text(s_status, msg);
        lv_obj_set_style_text_color(s_status, lv_color_hex(THEME_SUCCESS), 0);
    } else {
        lv_label_set_text(s_btn_label, LV_SYMBOL_WIFI "  Start Access Point");
        lv_obj_set_style_bg_color(s_btn, lv_color_hex(THEME_PRIMARY), 0);  // blue = off
        lv_label_set_text(s_status, "Access point is off.");
        lv_obj_set_style_text_color(s_status, lv_color_hex(THEME_MUTED), 0);
    }
    ui_state_set_wifi_ap(running);  // top-bar WiFi icon turns green
    shell_set_nav_enabled(&s_sh, !running || s_background);  // trap here unless allowed to roam
    sync_file_server(running);                               // serve the file manager while up
}

static void toggle_cb(lv_event_t*) {
    if (wifi_ap_is_running()) {
        wifi_ap_stop();
        refresh_state();
        return;
    }

    // WiFi.softAP() blocks the (single) LVGL thread for a moment, so a plain
    // label update wouldn't paint until after the freeze. Show a "turning on"
    // notice and force a synchronous redraw first, so the pause never looks
    // like a crash.
    lv_label_set_text(s_status, LV_SYMBOL_WIFI "  Turning on WiFi...");
    lv_obj_set_style_text_color(s_status, lv_color_hex(THEME_MUTED), 0);
    lv_refr_now(NULL);

    if (!wifi_ap_start()) {
        lv_label_set_text(s_status, LV_SYMBOL_WARNING " Failed to start (WiFi companion available?)");
        lv_obj_set_style_text_color(s_status, lv_color_hex(THEME_DANGER), 0);
        return;
    }
    refresh_state();
}

// One "LABEL: value" row: small muted caption over a large clear-text value.
static lv_obj_t* cred_value(lv_obj_t* parent, const char* caption) {
    ui_make_label(parent, caption, THEME_MUTED, &lv_font_montserrat_14);
    lv_obj_t* v = ui_make_label(parent, "", THEME_TEXT, &lv_font_montserrat_32);
    lv_label_set_long_mode(v, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(v, LV_PCT(100));
    return v;
}

static lv_obj_t* create(void) {
    s_sh = shell_create("Wifi File Editor", "Debug access point", true);
    shell_set_active_nav(&s_sh, SCREEN_WIFI_EDITOR);

    lv_obj_t* body = lv_obj_create(s_sh.body);
    lv_obj_remove_style_all(body);
    lv_obj_set_size(body, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(body, 12, 0);

    lv_obj_t* intro = ui_make_card(body);
    lv_obj_t* it = ui_make_label(
        intro, "Start the access point, then connect from a computer to edit files (debug).",
        THEME_MUTED, &lv_font_montserrat_14);
    lv_label_set_long_mode(it, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(it, LV_PCT(100));

    // Credentials, shown in clear text on purpose (debug).
    lv_obj_t* creds = ui_make_card(body);
    lv_obj_set_flex_flow(creds, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(creds, 8, 0);
    s_ssid_val = cred_value(creds, "Network (SSID)");
    s_pass_val = cred_value(creds, "Password");

    s_btn = ui_make_button(body, "", &theme_style_btn_primary, toggle_cb, nullptr);
    lv_obj_set_width(s_btn, LV_PCT(100));
    s_btn_label = lv_obj_get_child(s_btn, 0);

    // Debug option, off at every boot on purpose: the file manager has no
    // password, so the ability to walk away from it is never the default and
    // never persists.
    lv_obj_t* bg = ui_make_card(body);
    lv_obj_set_flex_flow(bg, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(bg, 8, 0);

    lv_obj_t* row = lv_obj_create(bg);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    ui_make_label(row, "Keep serving in the background", THEME_TEXT, &lv_font_montserrat_14);
    lv_obj_t* sw = lv_switch_create(row);
    lv_obj_add_event_cb(sw, background_toggle_cb, LV_EVENT_VALUE_CHANGED, nullptr);

    lv_obj_t* cap = ui_make_label(
        bg,
        "Debug: leaves the access point up while you use the rest of the device "
        "(kiosk, roll call). The file manager has no password — signing out stops it, "
        "and this switch resets when the device restarts.",
        THEME_MUTED, &lv_font_montserrat_14);
    lv_label_set_long_mode(cap, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(cap, LV_PCT(100));

    // The IP/status line: big and centered so the address is easy to read.
    s_status = ui_make_label(body, "", THEME_MUTED, &lv_font_montserrat_32);
    lv_label_set_long_mode(s_status, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_status, LV_PCT(100));
    lv_obj_set_style_text_align(s_status, LV_TEXT_ALIGN_CENTER, 0);

    return s_sh.root;
}

static void on_show(void*) {
    char ssid[33], pass[16];
    wifi_ap_credentials(ssid, sizeof(ssid), pass, sizeof(pass));
    lv_label_set_text(s_ssid_val, ssid);
    lv_label_set_text(s_pass_val, pass);
    refresh_state();
}

void scr_wifi_editor_stop_ap(void) {
    if (!wifi_ap_is_running() && !file_server_running()) return;
    wifi_ap_stop();
    if (s_sh.root) {
        refresh_state();  // also stops the server and repaints button/status/nav
    } else {
        // The AP can only be started from this screen, so create() has run by
        // now — but never dereference the widgets to prove it.
        sync_file_server(false);
        ui_state_set_wifi_ap(false);
    }
}

const screen_t scr_wifi_editor = {
    .create = create,
    .on_show = on_show,
    .on_hide = nullptr,
};
