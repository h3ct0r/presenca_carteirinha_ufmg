#include "ui/screens/scr_idle.h"

#include <stdio.h>

#include "app/auth.h"
#include "app/session.h"
#include "app/teacher.h"
#include "app/version.h"
#include "audio/beeper.h"
#include "services/config_service.h"
#include "services/import_service.h"
#include "services/roster_service.h"
#include "ui/components/keyboard.h"
#include "ui/components/toast.h"
#include "ui/screen_manager.h"
#include "ui/screens/scr_wifi_editor.h"
#include "ui/sd_resync.h"
#include "ui/theme/theme.h"
#include "ui/ui_state.h"

static constexpr uint32_t COL_BG = THEME_DARK_BG;
static constexpr uint32_t COL_TEXT = THEME_DARK_TEXT;
static constexpr uint32_t COL_MUTED = THEME_DARK_MUTED;
static constexpr uint32_t COL_ACCENT = THEME_DARK_ACCENT;
static constexpr uint32_t COL_WARN = THEME_DARK_WARN;
static constexpr uint32_t COL_OK = THEME_DARK_OK;

// Where a successful unlock lands: the operator's app.
static constexpr screen_id_t UNLOCK_TARGET = SCREEN_CLASSES;

static lv_obj_t* s_root = nullptr;
static lv_obj_t* s_prompt = nullptr;  // "tap your card"
static lv_obj_t* s_error = nullptr;   // "insert SD / config.json"
static lv_obj_t* s_error_title = nullptr;
static lv_obj_t* s_error_msg = nullptr;
static lv_obj_t* s_pw_button = nullptr;
static lv_obj_t* s_pw_alert = nullptr;  // shown instead of the button when no password
static lv_obj_t* s_denied = nullptr;    // transient "access denied" pill
static lv_timer_t* s_denied_timer = nullptr;
static lv_obj_t* s_pw_modal = nullptr;
static lv_obj_t* s_pw_ta = nullptr;
static lv_obj_t* s_import_btn = nullptr;    // "Import config from SD" (in the error panel)
static lv_obj_t* s_import_modal = nullptr;  // confirm overlay
static lv_obj_t* s_setup_btn = nullptr;     // "Set up this device" (no config.json at all)
static lv_obj_t* s_setup_modal = nullptr;
static lv_obj_t* s_setup_name_ta = nullptr;
static lv_obj_t* s_setup_pw_ta = nullptr;

// Montserrat with FontAwesome glyphs merged in (see docs/software/
// CUSTOM_FONT_GENERATION.md): U+F023 lock, U+F09C unlock,
// U+E595 users-viewfinder.
LV_FONT_DECLARE(font_montserrat_custom_14);
LV_FONT_DECLARE(font_montserrat_custom_20);

// ---- access outcomes ------------------------------------------------------

static void close_pw_modal(void) {
    if (s_pw_modal) {
        keyboard_hide();
        lv_obj_delete(s_pw_modal);
        s_pw_modal = nullptr;
        s_pw_ta = nullptr;
    }
}

static void hide_denied_cb(lv_timer_t* t) {
    lv_obj_add_flag(s_denied, LV_OBJ_FLAG_HIDDEN);
    lv_timer_delete(t);
    s_denied_timer = nullptr;
}

static void deny(void) {
    beeper_error();  // wrong card or password
    lv_obj_remove_flag(s_denied, LV_OBJ_FLAG_HIDDEN);
    if (s_denied_timer) lv_timer_delete(s_denied_timer);
    s_denied_timer = lv_timer_create(hide_denied_cb, 2500, nullptr);
}

// Starts a session for the given professor and opens the app. t is copied by
// session_set, so a stack local is fine.
static void enter_app(const teacher_t* t) {
    beeper_beep();  // access granted
    session_set(t);
    ui_state_set_user(t->name);
    close_pw_modal();
    if (s_denied_timer) {
        lv_timer_delete(s_denied_timer);
        s_denied_timer = nullptr;
    }
    scr_mgr_show(UNLOCK_TARGET, nullptr);
}

void scr_idle_handle_scan(const char* uid_hex) {
    if (config_get_status() != CONFIG_OK) return;  // nothing to authorize against
    teacher_t who;
    if (auth_lookup_uid(uid_hex, &who)) {
        enter_app(&who);
    } else {
        deny();
    }
}

// ---- password modal -------------------------------------------------------

static void pw_cancel_cb(lv_event_t*) { close_pw_modal(); }

static void pw_ok_cb(lv_event_t*) {
    const char* txt = s_pw_ta ? lv_textarea_get_text(s_pw_ta) : "";
    teacher_t who;
    if (auth_lookup_password(txt, &who)) {
        // Each password is unique, so it identifies the professor directly.
        enter_app(&who);  // closes the modal on the way
    } else {
        close_pw_modal();
        deny();
    }
}

static void open_pw_modal(lv_event_t*) {
    if (s_pw_modal || config_get_status() != CONFIG_OK) return;

    s_pw_modal = lv_obj_create(s_root);
    lv_obj_remove_style_all(s_pw_modal);
    lv_obj_set_size(s_pw_modal, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_pw_modal, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_pw_modal, LV_OPA_50, 0);
    lv_obj_add_flag(s_pw_modal, LV_OBJ_FLAG_CLICKABLE);  // swallow taps behind
    lv_obj_remove_flag(s_pw_modal, LV_OBJ_FLAG_SCROLLABLE);

    // Light card near the top, kept clear of the on-screen keyboard.
    lv_obj_t* card = ui_make_card(s_pw_modal);
    lv_obj_set_width(card, LV_PCT(86));
    lv_obj_align(card, LV_ALIGN_TOP_MID, 0, 70);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 12, 0);

    ui_make_label(card, "Enter password", THEME_PRIMARY, &lv_font_montserrat_32);

    // Passwords are digits-only (enforced at config load), so the login pad is
    // numeric.
    s_pw_ta = keyboard_make_textarea(card, "Password", 32, LV_KEYBOARD_MODE_NUMBER);
    lv_textarea_set_password_mode(s_pw_ta, true);

    // Pop the keypad up right away, and make its OK/finish button unlock — so
    // the professor can just type and press OK without extra taps.
    keyboard_show(s_pw_ta, LV_KEYBOARD_MODE_NUMBER);
    keyboard_set_ready_cb(pw_ok_cb);

    lv_obj_t* row = lv_obj_create(card);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 10, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* cancel = ui_make_button(row, "Cancel", &theme_style_btn_outline, pw_cancel_cb,
                                      nullptr);
    lv_obj_set_flex_grow(cancel, 1);
    lv_obj_t* ok = ui_make_button(row, "Unlock", &theme_style_btn_primary, pw_ok_cb, nullptr);
    lv_obj_set_flex_grow(ok, 1);
}

// ---- config import (offline config.tar) -----------------------------------

static void close_import_modal(void) {
    if (s_import_modal) {
        lv_obj_delete(s_import_modal);
        s_import_modal = nullptr;
    }
}

static void import_cancel_cb(lv_event_t*) { close_import_modal(); }

static void import_confirm_cb(lv_event_t*) {
    close_import_modal();
    // Runs the full pipeline (backup -> validate -> apply -> reload). On success
    // the reload republishes config/roster status, so the observers refresh the
    // gate panel on their own — nothing to do here but report.
    import_result_t r = import_service_run(import_service_tar_path());
    ui_toast_show(r.message, r.ok);
}

static void open_import_confirm(lv_event_t*) {
    if (s_import_modal) return;

    s_import_modal = lv_obj_create(s_root);
    lv_obj_remove_style_all(s_import_modal);
    lv_obj_set_size(s_import_modal, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_import_modal, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_import_modal, LV_OPA_50, 0);
    lv_obj_add_flag(s_import_modal, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(s_import_modal, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* card = ui_make_card(s_import_modal);
    lv_obj_set_width(card, LV_PCT(86));
    lv_obj_align(card, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 12, 0);

    ui_make_label(card, LV_SYMBOL_DOWNLOAD "  Import configuration", THEME_PRIMARY,
                  &lv_font_montserrat_20);
    lv_obj_t* hint = ui_make_label(
        card,
        "A config.tar was found on the SD card. Import it to set up this device? Any existing "
        "configuration is backed up first.",
        THEME_MUTED, &lv_font_montserrat_14);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(hint, LV_PCT(100));

    lv_obj_t* row = lv_obj_create(card);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 10, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t* cancel =
        ui_make_button(row, "Cancel", &theme_style_btn_outline, import_cancel_cb, nullptr);
    lv_obj_set_flex_grow(cancel, 1);
    lv_obj_t* ok = ui_make_button(row, "Import", &theme_style_btn_primary, import_confirm_cb,
                                  nullptr);
    lv_obj_set_flex_grow(ok, 1);
}

// ---- first-run setup (no config.json on the card) --------------------------

static void close_setup_modal(void) {
    if (s_setup_modal) {
        keyboard_hide();  // release before deleting its textareas (UAF guard)
        lv_obj_delete(s_setup_modal);
        s_setup_modal = nullptr;
        s_setup_name_ta = nullptr;
        s_setup_pw_ta = nullptr;
    }
}

static void setup_cancel_cb(lv_event_t*) { close_setup_modal(); }

static void setup_create_cb(lv_event_t*) {
    if (!s_setup_name_ta || !s_setup_pw_ta) return;
    config_result_t r = config_create_first_teacher(lv_textarea_get_text(s_setup_name_ta),
                                                    lv_textarea_get_text(s_setup_pw_ta));
    ui_toast_show(r.message, r.ok);
    // On success the reload republishes the config status, so the gate observer
    // swaps in the password button on its own — nothing to do here but close.
    // On failure the modal stays up with the typed values, ready to be fixed.
    if (r.ok) close_setup_modal();
}

// Offered only when the card has no config.json at all (see update_gate). Whoever
// is holding the device claims it by creating the first professor — the same
// trust-on-first-use a router's setup page uses, and no weaker than the card
// slot itself, which anyone present could write directly.
static void open_setup_modal(lv_event_t*) {
    if (s_setup_modal || config_get_status() != CONFIG_NO_FILE) return;

    s_setup_modal = lv_obj_create(s_root);
    lv_obj_remove_style_all(s_setup_modal);
    lv_obj_set_size(s_setup_modal, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_setup_modal, lv_color_hex(THEME_SCRIM), 0);
    lv_obj_set_style_bg_opa(s_setup_modal, LV_OPA_50, 0);
    lv_obj_add_flag(s_setup_modal, LV_OBJ_FLAG_CLICKABLE);  // swallow taps behind
    lv_obj_remove_flag(s_setup_modal, LV_OBJ_FLAG_SCROLLABLE);

    // Near the top, like the password modal: the keyboard covers the lower half.
    lv_obj_t* card = ui_make_card(s_setup_modal);
    lv_obj_set_width(card, LV_PCT(86));
    lv_obj_align(card, LV_ALIGN_TOP_MID, 0, 40);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 10, 0);

    ui_make_label(card, "Set up this device", THEME_PRIMARY, &lv_font_montserrat_20);
    lv_obj_t* hint = ui_make_label(card,
                                   "Create the first professor so you can unlock the device and "
                                   "load a configuration. This account sees every class and is "
                                   "replaced when you import a config.tar.",
                                   THEME_MUTED, &lv_font_montserrat_14);
    ui_label_fit(hint);

    ui_make_label(card, "Your name", THEME_TEXT, &lv_font_montserrat_14);
    s_setup_name_ta = keyboard_make_textarea(card, "Name", 47, LV_KEYBOARD_MODE_TEXT_UPPER);

    ui_make_label(card, "Password (digits only)", THEME_TEXT, &lv_font_montserrat_14);
    s_setup_pw_ta = keyboard_make_textarea(card, "Password", 31, LV_KEYBOARD_MODE_NUMBER);
    lv_textarea_set_password_mode(s_setup_pw_ta, true);

    lv_obj_t* row = lv_obj_create(card);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 10, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t* cancel = ui_make_button(row, "Cancel", &theme_style_btn_outline, setup_cancel_cb,
                                      nullptr);
    lv_obj_set_flex_grow(cancel, 1);
    lv_obj_t* ok = ui_make_button(row, "Create", &theme_style_btn_primary, setup_create_cb,
                                  nullptr);
    lv_obj_set_flex_grow(ok, 1);
}

// ---- static content -------------------------------------------------------

static lv_obj_t* make_card_glyph(lv_obj_t* parent) {
    lv_obj_t* card = lv_obj_create(parent);
    lv_obj_set_size(card, 210, 132);
    lv_obj_set_style_radius(card, 16, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(COL_ACCENT), 0);
    lv_obj_set_style_border_width(card, 3, 0);
    lv_obj_set_style_pad_all(card, 16, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* photo = lv_obj_create(card);
    lv_obj_set_size(photo, 52, 62);
    lv_obj_set_style_radius(photo, 8, 0);
    lv_obj_set_style_bg_color(photo, lv_color_hex(COL_ACCENT), 0);
    lv_obj_set_style_bg_opa(photo, LV_OPA_50, 0);
    lv_obj_set_style_border_width(photo, 0, 0);
    lv_obj_align(photo, LV_ALIGN_LEFT_MID, 0, 0);

    for (int i = 0; i < 3; i++) {
        lv_obj_t* bar = lv_obj_create(card);
        lv_obj_set_size(bar, i == 2 ? 60 : 96, 10);
        lv_obj_set_style_radius(bar, 5, 0);
        lv_obj_set_style_bg_color(bar, lv_color_hex(COL_MUTED), 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_60, 0);
        lv_obj_set_style_border_width(bar, 0, 0);
        lv_obj_align(bar, LV_ALIGN_TOP_RIGHT, 0, 6 + i * 24);
    }
    return card;
}

static lv_obj_t* make_panel(lv_obj_t* parent) {
    lv_obj_t* p = lv_obj_create(parent);
    lv_obj_remove_style_all(p);
    lv_obj_set_size(p, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(p, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(p, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(p, 24, 0);
    lv_obj_remove_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    return p;
}

static void build_prompt(lv_obj_t* root) {
    s_prompt = make_panel(root);
    make_card_glyph(s_prompt);

    lv_obj_t* title = lv_label_create(s_prompt);
    lv_label_set_text(title, "Tap your card");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_32, 0);

    lv_obj_t* hint = lv_label_create(s_prompt);
    lv_label_set_text(hint, "Tap the authorized card to unlock this device");
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(hint, lv_color_hex(COL_MUTED), 0);
}

static void build_error(lv_obj_t* root) {
    s_error = make_panel(root);
    lv_obj_set_style_pad_hor(s_error, 24, 0);

    lv_obj_t* icon = lv_label_create(s_error);
    lv_label_set_text(icon, LV_SYMBOL_SD_CARD);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_32, 0);
    lv_obj_set_style_text_color(icon, lv_color_hex(COL_WARN), 0);

    s_error_title = lv_label_create(s_error);
    lv_obj_set_style_text_font(s_error_title, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_error_title, lv_color_hex(COL_TEXT), 0);

    s_error_msg = lv_label_create(s_error);
    lv_obj_set_style_text_font(s_error_msg, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_error_msg, lv_color_hex(COL_MUTED), 0);
    lv_obj_set_style_text_align(s_error_msg, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_error_msg, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_error_msg, LV_PCT(90));

    // Offered only when a config.tar is waiting on the SD card (see update_gate).
    s_import_btn = ui_make_button(s_error, LV_SYMBOL_DOWNLOAD "  Import config from SD",
                                  &theme_style_btn_primary, open_import_confirm, nullptr);
    lv_obj_add_flag(s_import_btn, LV_OBJ_FLAG_HIDDEN);

    // The fallback when there is no tar either: claim the blank card on-device.
    // Secondary to the import — with a tar present that is the better path.
    s_setup_btn = ui_make_button(s_error, LV_SYMBOL_SETTINGS "  Set up this device",
                                 &theme_style_btn_outline, open_setup_modal, nullptr);
    lv_obj_add_flag(s_setup_btn, LV_OBJ_FLAG_HIDDEN);
}

// The gate panel reflects two independent checks: config.json (teachers /
// password) and the student/class data layout. Config problems win — the
// device is locked without them. Data problems are shown with the exact
// file/reason from the roster service, but staff can still unlock.
static void update_gate_cb(lv_observer_t*, lv_subject_t*) {
    int cfg = lv_subject_get_int(&ui_subj_config_status);
    int ros = lv_subject_get_int(&ui_subj_roster_status);
    bool cfg_ok = (cfg == CONFIG_OK);

    // Password entry needs a valid config AND at least one professor with a
    // password. With a valid config but no passwords, show an alert in the
    // button's place. With no valid config, the error panel below covers it.
    if (cfg_ok && config_has_any_password()) {
        lv_obj_remove_flag(s_pw_button, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_pw_alert, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_pw_button, LV_OBJ_FLAG_HIDDEN);
        close_pw_modal();
        if (cfg_ok) {
            lv_obj_remove_flag(s_pw_alert, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_pw_alert, LV_OBJ_FLAG_HIDDEN);
        }
    }

    if (cfg_ok && ros == ROSTER_OK) {
        lv_obj_remove_flag(s_prompt, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_error, LV_OBJ_FLAG_HIDDEN);
        close_setup_modal();  // never leave it stranded over the unlock prompt
        return;
    }

    lv_obj_add_flag(s_prompt, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_error, LV_OBJ_FLAG_HIDDEN);

    // Surface the one-tap SD import whenever a config.tar is waiting — the
    // primary path for setting up a brand-new device with no valid config yet.
    if (import_service_pending()) {
        lv_obj_remove_flag(s_import_btn, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_import_btn, LV_OBJ_FLAG_HIDDEN);
    }

    // The on-device bootstrap, offered for a missing config.json and nothing
    // else — a config that exists but fails to parse must be repaired, not
    // replaced. config_create_first_teacher() enforces this too; hiding the
    // button just keeps the offer honest.
    if (cfg == CONFIG_NO_FILE) {
        lv_obj_remove_flag(s_setup_btn, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_setup_btn, LV_OBJ_FLAG_HIDDEN);
        close_setup_modal();
    }

    const char* title = "Configuration needed";
    char msg[160];
    snprintf(msg, sizeof(msg), "Insert an SD card with a valid config.json file.");

    if (!cfg_ok) {
        switch (cfg) {
            case CONFIG_NO_SD:
                title = "No SD card";
                break;
            case CONFIG_NO_FILE:
                title = "config.json missing";
                break;
            case CONFIG_BAD_JSON:
                title = "Invalid config.json";
                break;
            case CONFIG_DUP_PASSWORD:
                title = "Duplicate password";
                break;
            case CONFIG_NON_NUMERIC_PASSWORD:
                title = "Password not numeric";
                break;
            default:
                break;
        }
        // Detailed, per-failure reason from the service (names the conflicting
        // professors for a duplicate password, the parse error otherwise).
        config_get_error(msg, sizeof(msg));
        if (msg[0] == '\0') snprintf(msg, sizeof(msg), "Check config.json on the SD card.");
    } else {
        // Config is fine; the student/class data is not. Show the service's
        // specific reason (names the file and the problem).
        switch (ros) {
            case ROSTER_NO_SD:
                title = "SD card problem";
                break;
            case ROSTER_NO_STUDENTS_FILE:
                title = "Student data missing";
                break;
            case ROSTER_BAD_STUDENTS:
                title = "Invalid students.json";
                break;
            case ROSTER_BAD_CLASS:
                title = "Invalid class data";
                break;
            default:
                title = "SD data error";
                break;
        }
        roster_get_error(msg, sizeof(msg));
        if (msg[0] == '\0') snprintf(msg, sizeof(msg), "Check the SD card data files.");
    }

    lv_label_set_text(s_error_title, title);
    lv_label_set_text(s_error_msg, msg);
}

static lv_obj_t* create(void) {
    s_root = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_root, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_text_color(s_root, lv_color_hex(COL_TEXT), 0);
    lv_obj_remove_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(s_root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_root, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    build_prompt(s_root);
    build_error(s_root);

    // Transient "access denied" pill, pinned below the status bar.
    s_denied = lv_obj_create(s_root);
    lv_obj_remove_style_all(s_denied);
    lv_obj_add_flag(s_denied, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_add_flag(s_denied, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_size(s_denied, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(s_denied, lv_color_hex(COL_WARN), 0);
    lv_obj_set_style_bg_opa(s_denied, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_denied, 10, 0);
    lv_obj_set_style_pad_hor(s_denied, 16, 0);
    lv_obj_set_style_pad_ver(s_denied, 8, 0);
    lv_obj_align(s_denied, LV_ALIGN_TOP_MID, 0, 50);
    lv_obj_t* dl = lv_label_create(s_denied);
    lv_label_set_text(dl, LV_SYMBOL_CLOSE "  Access denied");
    lv_obj_set_style_text_color(dl, lv_color_hex(0xFFFFFF), 0);

    // Password fallback, pinned to the bottom edge. A white rounded box so it
    // reads as a tappable button against the dark idle background.
    s_pw_button = lv_button_create(s_root);
    lv_obj_remove_style_all(s_pw_button);
    lv_obj_add_flag(s_pw_button, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_set_style_bg_color(s_pw_button, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(s_pw_button, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_pw_button, 10, 0);
    lv_obj_set_style_pad_all(s_pw_button, 12, 0);
    lv_obj_align(s_pw_button, LV_ALIGN_BOTTOM_MID, 0, -32);
    lv_obj_add_event_cb(s_pw_button, open_pw_modal, LV_EVENT_CLICKED, nullptr);
    ui_add_press_feedback(s_pw_button);
    lv_obj_t* pl = lv_label_create(s_pw_button);
    lv_label_set_text(pl, LV_SYMBOL_KEYBOARD "  Enter password");
    lv_obj_set_style_text_color(pl, lv_color_hex(THEME_PRIMARY), 0);
    lv_obj_set_style_text_font(pl, &lv_font_montserrat_14, 0);

    // Shown in the button's place when no professor has a password configured
    // (card is then the only way in). Same bottom anchor, hidden by default.
    s_pw_alert = lv_label_create(s_root);
    lv_obj_add_flag(s_pw_alert, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_add_flag(s_pw_alert, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(s_pw_alert, LV_SYMBOL_WARNING "  No password set in config.json - Using RFID only");
    lv_obj_set_style_text_color(s_pw_alert, lv_color_hex(COL_WARN), 0);
    lv_obj_set_style_text_font(s_pw_alert, &lv_font_montserrat_14, 0);
    lv_obj_align(s_pw_alert, LV_ALIGN_BOTTOM_MID, 0, -20);

    // Firmware version, bottom-right corner. Deliberately a plain label (not a
    // button): this screen is student-facing, so it answers "which build is this
    // device running?" without adding a tap target that leaves the check-in
    // flow. The full build id (with git SHA) lives on the About screen.
    lv_obj_t* ver = lv_label_create(s_root);
    lv_obj_add_flag(ver, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_label_set_text(ver, APP_VERSION_FULL);
    lv_obj_set_style_text_color(ver, lv_color_hex(COL_MUTED), 0);
    lv_obj_set_style_text_opa(ver, LV_OPA_60, 0);
    lv_obj_set_style_text_font(ver, &lv_font_montserrat_14, 0);
    lv_obj_align(ver, LV_ALIGN_BOTTOM_RIGHT, -10, -8);

    // Observers fire immediately with the current status, so the first
    // rendered frame already shows the right panel.
    lv_subject_add_observer(&ui_subj_config_status, update_gate_cb, nullptr);
    lv_subject_add_observer(&ui_subj_roster_status, update_gate_cb, nullptr);

    return s_root;
}

// Reaching the gate means nobody is signed in, so the debug file manager — which
// has no password of its own — must not still be serving. Covers the Admin
// logout and any future path back here; a no-op at boot.
static void on_show(void*) {
    scr_wifi_editor_stop_ap();
    ui_sd_resync_full();  // the web editor may have rewritten config/roster
}

static void on_hide(void) {
    close_pw_modal();
    close_import_modal();
    close_setup_modal();
    if (s_denied_timer) {
        lv_timer_delete(s_denied_timer);
        s_denied_timer = nullptr;
    }
    lv_obj_add_flag(s_denied, LV_OBJ_FLAG_HIDDEN);
}

const screen_t scr_idle = {
    .create = create,
    .on_show = on_show,
    .on_hide = on_hide,
};
