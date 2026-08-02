#include "ui/ui.h"

#include "esp32-hal-log.h"
#include "audio/beeper.h"
#include "ui/components/keyboard.h"
#include "ui/components/status_bar.h"
#include "ui/components/toast.h"
#include "ui/lvgl_fs_sd.h"
#include "ui/screen_manager.h"
#include "ui/screens/scr_admin.h"
#include "ui/screens/scr_about.h"
#include "ui/screens/scr_class.h"
#include "ui/screens/scr_class_stats.h"
#include "ui/screens/scr_camera.h"
#include "ui/screens/scr_classes.h"
#include "ui/screens/scr_export.h"
#include "ui/screens/scr_idle.h"
#include "ui/screens/scr_wifi_editor.h"
#include "ui/screens/scr_kiosk.h"
#include "ui/screens/scr_students.h"
#include "ui/theme/theme.h"
#include "ui/ui_state.h"

static const char* UI_TAG = "ui";

static ui_card_cb_t s_card_capture = nullptr;

void ui_set_card_capture(ui_card_cb_t cb) { s_card_capture = cb; }

void ui_init(void) {
    theme_init();
    lvgl_fs_sd_init();  // 'S' drive: load student avatars off the SD card
    ui_state_init();
    status_bar_create();
    keyboard_create();  // after the status bar so it stacks above on layer_top

    scr_mgr_register(SCREEN_IDLE, &scr_idle);
    scr_mgr_register(SCREEN_CLASSES, &scr_classes);
    scr_mgr_register(SCREEN_CLASS, &scr_class);
    scr_mgr_register(SCREEN_CLASS_STATS, &scr_class_stats);
    scr_mgr_register(SCREEN_STUDENTS, &scr_students);
    scr_mgr_register(SCREEN_ADMIN, &scr_admin);
    scr_mgr_register(SCREEN_KIOSK, &scr_kiosk);
    scr_mgr_register(SCREEN_EXPORT, &scr_export);
    scr_mgr_register(SCREEN_WIFI_EDITOR, &scr_wifi_editor);
    scr_mgr_register(SCREEN_CAMERA, &scr_camera);
    scr_mgr_register(SCREEN_ABOUT, &scr_about);
    scr_mgr_show(SCREEN_IDLE, nullptr);
}

void ui_handle_event(const app_event_t* ev) {
    switch (ev->type) {
        case APP_EVENT_CARD_SCANNED: {
            char uid_hex[32];
            app_event_uid_to_hex(ev->card.uid, ev->card.uid_len, uid_hex, sizeof(uid_hex));
            // A flow waiting to read a card (e.g. enroll) takes priority.
            if (s_card_capture) {
                ui_card_cb_t cb = s_card_capture;
                s_card_capture = nullptr;  // one-shot
                cb(uid_hex);
                break;
            }
            // Otherwise, on the idle page a scan is an access attempt.
            if (scr_mgr_current() == SCREEN_IDLE) {
                scr_idle_handle_scan(uid_hex);
                break;
            }
            // Nothing was listening. On kiosk/enroll that means a flow disarmed
            // the one-shot capture and never re-armed it, so the screen looks
            // dead while the RFID task keeps logging every tap. Say so — this
            // silence is exactly what made a stuck kiosk impossible to diagnose.
            ESP_LOGW(UI_TAG, "card %s ignored: no capture armed on screen %d", uid_hex,
                     (int)scr_mgr_current());
            break;
        }
        case APP_EVENT_CARD_COLLISION:
            // Screen-agnostic on purpose: this can happen on the idle gate, the
            // roll call or the kiosk, and the advice is the same everywhere.
            beeper_error();
            ui_toast_show("Two cards detected - present one card at a time", false);
            break;
        case APP_EVENT_NET_STATE:
            ui_state_set_net(ev->net.connected, ev->net.rssi);
            break;
        case APP_EVENT_POWER_STATE:
            ui_state_set_power(ev->power.pct, ev->power.mv);
            break;
        case APP_EVENT_CONFIG_STATE:
            ui_state_set_config(ev->config.status);
            break;
        case APP_EVENT_ROSTER_STATE:
            ui_state_set_roster(ev->roster.status);
            break;
    }
}
