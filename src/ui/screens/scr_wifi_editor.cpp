#include "ui/screens/scr_wifi_editor.h"

#include <stdio.h>

#include "services/file_server.h"
#include "services/wifi_ap.h"
#include "ui/components/shell.h"
#include "ui/theme/theme.h"
#include "ui/ui_state.h"

static shell_t s_sh;
static lv_obj_t* s_ssid_val = nullptr;
static lv_obj_t* s_pass_val = nullptr;
static lv_obj_t* s_btn = nullptr;
static lv_obj_t* s_btn_label = nullptr;
static lv_obj_t* s_status = nullptr;
static lv_timer_t* s_poll = nullptr;  // pumps the HTTP server on the LVGL thread

// The sync web server must be serviced often; do it from an LVGL timer so all
// its SD I/O stays on this (the only SD-touching) thread.
static void poll_cb(lv_timer_t*) { file_server_handle(); }

// Starts/stops the file server + its poll timer to match the AP state.
static void sync_file_server(bool running) {
    if (running) {
        if (!file_server_running()) file_server_begin();
        if (!s_poll) s_poll = lv_timer_create(poll_cb, 5, nullptr);
    } else {
        if (s_poll) {
            lv_timer_delete(s_poll);
            s_poll = nullptr;
        }
        if (file_server_running()) file_server_end();
    }
}

// Reflects the current AP state onto the button (green when live), the status
// line (the IP, big + centered), the top-bar WiFi icon, and the bottom nav
// (locked while the AP is up, so it can't be left running on another screen).
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
    ui_state_set_wifi_ap(running);            // top-bar WiFi icon turns green
    shell_set_nav_enabled(&s_sh, !running);   // trap here until the AP is stopped
    sync_file_server(running);                // serve the file manager while up
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

const screen_t scr_wifi_editor = {
    .create = create,
    .on_show = on_show,
    .on_hide = nullptr,
};
