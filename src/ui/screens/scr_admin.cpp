#include "ui/screens/scr_admin.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp32-hal-log.h"

#include "app/session.h"
#include "audio/beeper.h"
#include "services/config_service.h"
#include "services/import_service.h"
#include "services/roster_service.h"
#include "storage/attendance_store.h"
#include "storage/backup_store.h"
#include "storage/sd_card.h"
#include "storage/sd_tree.h"
#include "ui/components/keyboard.h"
#include "ui/components/shell.h"
#include "ui/components/toast.h"
#include "app/version.h"
#include "ui/screen_manager.h"
#include "ui/theme/theme.h"
#include "ui/ui.h"
#include "ui/ui_state.h"

// Profile identity labels, filled in on_show from the logged-in session.
static lv_obj_t* s_name = nullptr;
static lv_obj_t* s_email = nullptr;
static lv_obj_t* s_card = nullptr;

// Rebuilt-on-show section containers, and the modal parent.
static lv_obj_t* s_root = nullptr;      // shell root, parent for the modals
static lv_obj_t* s_storage = nullptr;   // SD-card usage meter
static lv_obj_t* s_security = nullptr;  // password section
static lv_obj_t* s_rfid = nullptr;      // professor RFID card section
static lv_obj_t* s_settings = nullptr;  // device settings (photo capture)
static lv_obj_t* s_debug = nullptr;     // debug tools section

// Debug wipe controls. Both buttons are destructive and hidden until the debug
// toggle is on; the second one takes the whole card, not just the roster data.
static lv_obj_t* s_wipe_btn = nullptr;      // destructive button, shown only in debug mode
static lv_obj_t* s_wipe_confirm = nullptr;  // wipe confirmation overlay
static lv_obj_t* s_card_wipe_btn = nullptr;      // "erase the whole card"
static lv_obj_t* s_card_wipe_confirm = nullptr;  // its confirmation overlay

// Password modal.
static lv_obj_t* s_pw_modal = nullptr;
static lv_obj_t* s_pw_new = nullptr;
static lv_obj_t* s_pw_confirm = nullptr;

// "Tap the new card" modal for changing the professor's own RFID card.
static lv_obj_t* s_rfid_modal = nullptr;

// Debug card-reader modal (prints whatever card is tapped).
static lv_obj_t* s_reader_modal = nullptr;
static lv_obj_t* s_reader_uid = nullptr;  // label that shows the last UID read

// Config-import section + shared confirm overlay (import / revert).
static lv_obj_t* s_import = nullptr;
static lv_obj_t* s_confirm_modal = nullptr;
static void (*s_confirm_action)(void) = nullptr;

static void build_security(void);
static void build_rfid(void);
static void build_storage(void);
static void build_import(void);
static void fill_profile(void);
static void import_btn_cb(lv_event_t*);
static void revert_btn_cb(lv_event_t*);

static void sign_out_cb(lv_event_t*) {
    session_set(nullptr);
    ui_state_set_user(nullptr);  // top bar back to the device name
    scr_mgr_show(SCREEN_IDLE, nullptr);
}

// True if the logged-in professor already has a password in config.json.
static bool current_has_password(void) {
    const teacher_t* t = session_get();
    if (!t) return false;
    return config_teacher_has_password(t->email, t->rfid_uid);
}

// --- password modal ---------------------------------------------------------

static void close_pw_modal(void) {
    if (s_pw_modal) {
        keyboard_hide();
        lv_obj_delete(s_pw_modal);
        s_pw_modal = nullptr;
        s_pw_new = nullptr;
        s_pw_confirm = nullptr;
    }
}

static void pw_cancel_cb(lv_event_t*) { close_pw_modal(); }

static void pw_save_cb(lv_event_t*) {
    const char* a = s_pw_new ? lv_textarea_get_text(s_pw_new) : "";
    const char* b = s_pw_confirm ? lv_textarea_get_text(s_pw_confirm) : "";
    if (strcmp(a, b) != 0) {
        ui_toast_show("Passwords do not match", false);
        return;
    }
    const teacher_t* t = session_get();
    config_result_t r = config_set_password(t ? t->email : "", t ? t->rfid_uid : "", a);
    ui_toast_show(r.message, r.ok);
    if (r.ok) {
        close_pw_modal();
        build_security();  // refresh the "password set / not set" status
    }
}

static void open_pw_modal(void) {
    if (s_pw_modal) return;
    bool has = current_has_password();

    // Parented to the screen root (not lv_layer_top) so the shared password
    // keyboard, which lives on lv_layer_top, floats ABOVE this modal's dim
    // overlay instead of being covered by it.
    s_pw_modal = lv_obj_create(s_root);
    lv_obj_remove_style_all(s_pw_modal);
    // Escape the shell's flex flow so this is a true full-screen overlay.
    lv_obj_add_flag(s_pw_modal, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_set_size(s_pw_modal, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_pw_modal, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_pw_modal, LV_OPA_50, 0);
    lv_obj_add_flag(s_pw_modal, LV_OBJ_FLAG_CLICKABLE);  // swallow taps behind
    lv_obj_remove_flag(s_pw_modal, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* card = ui_make_card(s_pw_modal);
    lv_obj_set_width(card, LV_PCT(88));
    lv_obj_align(card, LV_ALIGN_TOP_MID, 0, 60);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 12, 0);

    ui_make_label(card, has ? "Change password" : "Set password", THEME_PRIMARY,
                  &lv_font_montserrat_20);
    lv_obj_t* hint = ui_make_label(card, "Digits only. This is your login fallback.",
                                   THEME_MUTED, &lv_font_montserrat_14);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(hint, LV_PCT(100));

    s_pw_new = keyboard_make_textarea(card, "New password", 31, LV_KEYBOARD_MODE_NUMBER);
    lv_textarea_set_password_mode(s_pw_new, true);
    s_pw_confirm = keyboard_make_textarea(card, "Confirm password", 31, LV_KEYBOARD_MODE_NUMBER);
    lv_textarea_set_password_mode(s_pw_confirm, true);

    // Pop the numeric pad up on the first field right away (matches the idle
    // login / class-unlock modals) so no extra tap is needed to start typing.
    keyboard_show(s_pw_new, LV_KEYBOARD_MODE_NUMBER);

    lv_obj_t* row = lv_obj_create(card);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 10, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t* cancel = ui_make_button(row, "Cancel", &theme_style_btn_outline, pw_cancel_cb,
                                      nullptr);
    lv_obj_set_flex_grow(cancel, 1);
    lv_obj_t* save = ui_make_button(row, "Save", &theme_style_btn_primary, pw_save_cb, nullptr);
    lv_obj_set_flex_grow(save, 1);
}

static void change_pw_cb(lv_event_t*) { open_pw_modal(); }

// --- profile ----------------------------------------------------------------

static void fill_profile(void) {
    const teacher_t* t = session_get();
    lv_label_set_text(s_name, t ? t->name : "—");

    char line[96];
    snprintf(line, sizeof(line), "Email: %s", (t && t->email[0]) ? t->email : "(none)");
    lv_label_set_text(s_email, line);

    if (t && t->rfid_uid[0]) {
        snprintf(line, sizeof(line), "Card: %s", t->rfid_uid);
        lv_obj_set_style_text_color(s_card, lv_color_hex(THEME_SUCCESS), 0);
    } else {
        snprintf(line, sizeof(line), "Card: (password login)");
        lv_obj_set_style_text_color(s_card, lv_color_hex(THEME_MUTED), 0);
    }
    lv_label_set_text(s_card, line);
}

// --- SD-card usage ----------------------------------------------------------

// "3.6 GB" / "740 MB" from a raw byte count.
static void fmt_bytes(unsigned long long b, char* out, size_t cap) {
    double gb = (double)b / (1024.0 * 1024.0 * 1024.0);
    if (gb >= 1.0) {
        snprintf(out, cap, "%.1f GB", gb);
    } else {
        snprintf(out, cap, "%.0f MB", (double)b / (1024.0 * 1024.0));
    }
}

static void build_storage(void) {
    lv_obj_clean(s_storage);

    lv_obj_t* card = ui_make_card(s_storage);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 8, 0);
    ui_make_label(card, "SD Card", THEME_PRIMARY, &lv_font_montserrat_20);

    unsigned long long total = 0, used = 0;
    if (!sd_card_usage(&total, &used) || total == 0) {
        ui_make_label(card, "Card not available.", THEME_MUTED, &lv_font_montserrat_14);
        return;
    }
    int pct = (int)((used * 100ULL) / total);

    lv_obj_t* bar = lv_bar_create(card);
    lv_obj_set_size(bar, LV_PCT(100), 14);
    lv_bar_set_range(bar, 0, 100);
    lv_bar_set_value(bar, pct, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bar, lv_color_hex(THEME_BORDER), LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, lv_color_hex(pct >= 90 ? THEME_DANGER : THEME_SUCCESS),
                              LV_PART_INDICATOR);

    char used_s[16], total_s[16], line[64];
    fmt_bytes(used, used_s, sizeof(used_s));
    fmt_bytes(total, total_s, sizeof(total_s));
    snprintf(line, sizeof(line), "%s of %s used (%d%%)", used_s, total_s, pct);
    ui_make_label(card, line, THEME_MUTED, &lv_font_montserrat_14);
}

// --- password section -------------------------------------------------------

static void build_security(void) {
    lv_obj_clean(s_security);
    bool has = current_has_password();

    lv_obj_t* card = ui_make_card(s_security);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 8, 0);

    ui_make_label(card, "Password", THEME_PRIMARY, &lv_font_montserrat_20);
    ui_make_label(card,
                  has ? "A password is set for your account."
                      : "No password set. Add one to log in without your card.",
                  has ? THEME_SUCCESS : THEME_MUTED, &lv_font_montserrat_14);

    lv_obj_t* btn = ui_make_button(card, has ? "Change password" : "Set password",
                                   &theme_style_btn_primary, change_pw_cb, nullptr);
    lv_obj_set_width(btn, LV_PCT(100));
}

// --- professor RFID card ----------------------------------------------------

static void close_rfid_modal(void) {
    if (s_rfid_modal) {
        ui_set_card_capture(nullptr);  // stop diverting scans to us
        lv_obj_delete(s_rfid_modal);
        s_rfid_modal = nullptr;
    }
}

// Runs on the LVGL thread when a card is tapped while the modal is open.
static void on_rfid_card(const char* uid_hex) {
    const teacher_t* t = session_get();
    config_result_t r = config_set_rfid(t ? t->email : "", t ? t->rfid_uid : "", uid_hex);
    // Audible either way: you are holding a card to the reader, not watching the
    // screen, so the tone is what tells you the rebind landed.
    if (r.ok) {
        beeper_beep();
    } else {
        beeper_error();
    }
    ui_toast_show(r.message, r.ok);
    if (r.ok) {
        // Keep the session identity in sync so a later change still matches us
        // (the rfid_uid fallback identity would otherwise use the stale UID).
        if (t) {
            teacher_t updated = *t;
            snprintf(updated.rfid_uid, sizeof(updated.rfid_uid), "%s", uid_hex);
            session_set(&updated);
        }
        close_rfid_modal();
        fill_profile();  // profile card shows the card too
        build_rfid();    // refresh the "card set" status
    } else {
        ui_set_card_capture(on_rfid_card);  // let them try another card
    }
}

static void rfid_cancel_cb(lv_event_t*) { close_rfid_modal(); }

static void open_rfid_modal(void) {
    if (s_rfid_modal) return;

    s_rfid_modal = lv_obj_create(s_root);
    lv_obj_remove_style_all(s_rfid_modal);
    lv_obj_add_flag(s_rfid_modal, LV_OBJ_FLAG_IGNORE_LAYOUT);  // full-screen overlay
    lv_obj_set_size(s_rfid_modal, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_rfid_modal, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_rfid_modal, LV_OPA_50, 0);
    lv_obj_add_flag(s_rfid_modal, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(s_rfid_modal, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* card = ui_make_card(s_rfid_modal);
    lv_obj_set_width(card, LV_PCT(88));
    lv_obj_align(card, LV_ALIGN_TOP_MID, 0, 80);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(card, 12, 0);
    lv_obj_set_style_pad_ver(card, 20, 0);

    ui_make_label(card, LV_SYMBOL_SD_CARD, THEME_ACCENT, &lv_font_montserrat_32);
    ui_make_label(card, "Tap your new card", THEME_PRIMARY, &lv_font_montserrat_20);
    lv_obj_t* hint = ui_make_label(
        card, "Hold the RFID card near the reader. It replaces your current login card.",
        THEME_MUTED, &lv_font_montserrat_14);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(hint, LV_PCT(100));

    lv_obj_t* cancel = ui_make_button(card, "Cancel", &theme_style_btn_outline, rfid_cancel_cb,
                                      nullptr);
    lv_obj_set_width(cancel, LV_PCT(100));

    ui_set_card_capture(on_rfid_card);  // divert the next scan here
}

static void change_rfid_cb(lv_event_t*) { open_rfid_modal(); }

static void build_rfid(void) {
    lv_obj_clean(s_rfid);
    const teacher_t* t = session_get();
    bool has = t && t->rfid_uid[0];

    lv_obj_t* card = ui_make_card(s_rfid);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 8, 0);

    ui_make_label(card, "RFID card", THEME_PRIMARY, &lv_font_montserrat_20);
    if (has) {
        char line[80];
        snprintf(line, sizeof(line), "Your login card: %s", t->rfid_uid);
        ui_make_label(card, line, THEME_SUCCESS, &lv_font_montserrat_14);
    } else {
        ui_make_label(card, "No card bound. Add one to log in by tapping instead of typing.",
                      THEME_MUTED, &lv_font_montserrat_14);
    }

    lv_obj_t* btn = ui_make_button(card, has ? "Change card" : "Set card",
                                   &theme_style_btn_primary, change_rfid_cb, nullptr);
    lv_obj_set_width(btn, LV_PCT(100));
}

// --- device settings --------------------------------------------------------

static void camera_preview_cb(lv_event_t*) { scr_mgr_show(SCREEN_CAMERA, nullptr); }
static void about_cb(lv_event_t*) { scr_mgr_show(SCREEN_ABOUT, nullptr); }

static void build_settings(void) {
    lv_obj_clean(s_settings);

    lv_obj_t* card = ui_make_card(s_settings);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 8, 0);
    ui_make_label(card, "Camera", THEME_PRIMARY, &lv_font_montserrat_20);

    lv_obj_t* cap = ui_make_label(
        card, "Photo check-in (face verify) is configured per class in its settings. Use the "
              "preview to aim and test the camera.",
        THEME_MUTED, &lv_font_montserrat_14);
    lv_label_set_long_mode(cap, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(cap, LV_PCT(100));

    lv_obj_t* preview = ui_make_button(card, LV_SYMBOL_IMAGE "  Camera preview",
                                       &theme_style_btn_outline, camera_preview_cb, nullptr);
    lv_obj_set_width(preview, LV_PCT(100));
}

// --- debug tools ------------------------------------------------------------

// Performs the wipe: unbind every card + delete every class's attendance.
// Both halves report specifically — a failure must never surface as a bare
// "failed" with no cause (the details also go to the serial log).
static void do_wipe(void) {
    roster_result_t cards = roster_clear_all_uids();

    int files = 0, failed = 0;
    for (int i = 0; i < roster_class_count(); i++) {
        const class_rec_t* c = roster_class_at(i);
        if (!c) continue;
        int f = 0;
        files += attendance_clear(c->dir, &f);
        failed += f;
    }

    char msg[160];
    if (!cards.ok) {
        // The card wipe is the destructive half that can leave things
        // inconsistent, so its reason wins the toast.
        snprintf(msg, sizeof(msg), "Wipe failed: %s", cards.message);
    } else if (failed) {
        snprintf(msg, sizeof(msg), "%s, removed %d attendance file(s), %d could not be deleted",
                 cards.message, files, failed);
    } else {
        snprintf(msg, sizeof(msg), "%s, removed %d attendance file(s)", cards.message, files);
    }
    ui_toast_show(msg, cards.ok && failed == 0);
    build_storage();  // usage just dropped
}

static void close_wipe_confirm(void) {
    if (s_wipe_confirm) {
        lv_obj_delete(s_wipe_confirm);
        s_wipe_confirm = nullptr;
    }
}

static void wipe_cancel_cb(lv_event_t*) { close_wipe_confirm(); }

static void wipe_confirm_cb(lv_event_t*) {
    close_wipe_confirm();
    do_wipe();
}

static void open_wipe_confirm(void) {
    if (s_wipe_confirm) return;

    s_wipe_confirm = lv_obj_create(s_root);
    lv_obj_remove_style_all(s_wipe_confirm);
    lv_obj_add_flag(s_wipe_confirm, LV_OBJ_FLAG_IGNORE_LAYOUT);  // full-screen overlay
    lv_obj_set_size(s_wipe_confirm, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_wipe_confirm, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_wipe_confirm, LV_OPA_50, 0);
    lv_obj_add_flag(s_wipe_confirm, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(s_wipe_confirm, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* card = ui_make_card(s_wipe_confirm);
    lv_obj_set_width(card, LV_PCT(88));
    lv_obj_align(card, LV_ALIGN_TOP_MID, 0, 80);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 12, 0);

    ui_make_label(card, LV_SYMBOL_WARNING "  Delete all data?", THEME_DANGER,
                  &lv_font_montserrat_20);
    lv_obj_t* hint = ui_make_label(
        card, "This unbinds every student's RFID card and erases the attendance logs of "
              "every class. This cannot be undone.",
        THEME_MUTED, &lv_font_montserrat_14);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(hint, LV_PCT(100));

    lv_obj_t* row = lv_obj_create(card);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 10, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t* cancel = ui_make_button(row, "Cancel", &theme_style_btn_outline, wipe_cancel_cb,
                                      nullptr);
    lv_obj_set_flex_grow(cancel, 1);
    lv_obj_t* del = ui_make_button(row, "Delete all", &theme_style_btn_danger, wipe_confirm_cb,
                                   nullptr);
    lv_obj_set_flex_grow(del, 1);
}

static void wipe_cb(lv_event_t*) { open_wipe_confirm(); }

// --- erase the whole card (everything but /config.json) ----------------------

// The harder wipe: every file and folder at the card root goes except
// config.json, so the professors and their passwords survive and the device can
// still be signed into. Rosters, attendance, photos, exports, face models and
// the import backup are all destroyed.
static void do_card_wipe(void) {
    // Nothing may hold a session open on files that are about to vanish.
    attendance_close();

    sd_tree_stats_t st = {0, 0};
    char err[80] = "";
    bool ok = sd_tree_wipe_root("config.json", &st, err, sizeof(err));

    // The roster is gone from the card; drop it from RAM too, or the class list
    // keeps offering classes whose folders no longer exist.
    roster_service_reload();

    char msg[160];
    if (ok) {
        snprintf(msg, sizeof(msg), "Card erased: %d item(s) deleted, config.json kept",
                 st.removed);
    } else {
        snprintf(msg, sizeof(msg), "Card erase incomplete: %s", err);
    }
    ui_toast_show(msg, ok);
    build_storage();  // usage just dropped
}

static void close_card_wipe_confirm(void) {
    if (s_card_wipe_confirm) {
        lv_obj_delete(s_card_wipe_confirm);
        s_card_wipe_confirm = nullptr;
    }
}

static void card_wipe_cancel_cb(lv_event_t*) { close_card_wipe_confirm(); }

static void card_wipe_confirm_cb(lv_event_t*) {
    close_card_wipe_confirm();
    do_card_wipe();
}

static void open_card_wipe_confirm(void) {
    if (s_card_wipe_confirm) return;

    s_card_wipe_confirm = lv_obj_create(s_root);
    lv_obj_remove_style_all(s_card_wipe_confirm);
    lv_obj_add_flag(s_card_wipe_confirm, LV_OBJ_FLAG_IGNORE_LAYOUT);  // full-screen overlay
    lv_obj_set_size(s_card_wipe_confirm, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_card_wipe_confirm, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_card_wipe_confirm, LV_OPA_50, 0);
    lv_obj_add_flag(s_card_wipe_confirm, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(s_card_wipe_confirm, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* card = ui_make_card(s_card_wipe_confirm);
    lv_obj_set_width(card, LV_PCT(88));
    lv_obj_align(card, LV_ALIGN_TOP_MID, 0, 80);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 12, 0);

    ui_make_label(card, LV_SYMBOL_WARNING "  Erase the whole SD card?", THEME_DANGER,
                  &lv_font_montserrat_20);
    // Spell out what goes and what stays: this is the one action that also
    // takes the face-detection models, which need re-uploading afterwards.
    lv_obj_t* hint = ui_make_label(
        card,
        "Deletes EVERYTHING on the card except config.json: students, classes, all "
        "attendance, photos, CSV exports, the face-detection models and the import "
        "backup.\n\nProfessors and their passwords are kept. This cannot be undone.",
        THEME_MUTED, &lv_font_montserrat_14);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(hint, LV_PCT(100));

    lv_obj_t* row = lv_obj_create(card);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 10, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t* cancel = ui_make_button(row, "Cancel", &theme_style_btn_outline,
                                      card_wipe_cancel_cb, nullptr);
    lv_obj_set_flex_grow(cancel, 1);
    lv_obj_t* del = ui_make_button(row, "Erase card", &theme_style_btn_danger,
                                   card_wipe_confirm_cb, nullptr);
    lv_obj_set_flex_grow(del, 1);
}

static void card_wipe_cb(lv_event_t*) { open_card_wipe_confirm(); }

static void debug_toggle_cb(lv_event_t* e) {
    bool on = lv_obj_has_state((lv_obj_t*)lv_event_get_target(e), LV_STATE_CHECKED);
    if (on) {
        lv_obj_remove_flag(s_wipe_btn, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_card_wipe_btn, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_wipe_btn, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_card_wipe_btn, LV_OBJ_FLAG_HIDDEN);
    }
}

// --- debug card reader ------------------------------------------------------

static void close_reader_modal(void) {
    if (s_reader_modal) {
        ui_set_card_capture(nullptr);
        lv_obj_delete(s_reader_modal);
        s_reader_modal = nullptr;
        s_reader_uid = nullptr;
    }
}

// Prints each tapped card's UID and stays armed for the next one.
static void on_reader_card(const char* uid_hex) {
    ESP_LOGI("admin", "debug card reader: %s", uid_hex);
    if (s_reader_uid) {
        lv_label_set_text(s_reader_uid, uid_hex);
        lv_obj_set_style_text_color(s_reader_uid, lv_color_hex(THEME_SUCCESS), 0);
    }
    ui_set_card_capture(on_reader_card);  // keep reading
}

static void reader_close_cb(lv_event_t*) { close_reader_modal(); }

static void open_reader_modal(void) {
    if (s_reader_modal) return;

    s_reader_modal = lv_obj_create(s_root);
    lv_obj_remove_style_all(s_reader_modal);
    lv_obj_add_flag(s_reader_modal, LV_OBJ_FLAG_IGNORE_LAYOUT);  // full-screen overlay
    lv_obj_set_size(s_reader_modal, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_reader_modal, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_reader_modal, LV_OPA_50, 0);
    lv_obj_add_flag(s_reader_modal, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(s_reader_modal, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* card = ui_make_card(s_reader_modal);
    lv_obj_set_width(card, LV_PCT(88));
    lv_obj_align(card, LV_ALIGN_TOP_MID, 0, 80);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(card, 12, 0);
    lv_obj_set_style_pad_ver(card, 20, 0);

    ui_make_label(card, LV_SYMBOL_SD_CARD, THEME_ACCENT, &lv_font_montserrat_32);
    ui_make_label(card, "Card reader", THEME_PRIMARY, &lv_font_montserrat_20);
    ui_make_label(card, "Tap any card to read its UID", THEME_MUTED, &lv_font_montserrat_14);

    s_reader_uid = ui_make_label(card, "Waiting for card...", THEME_MUTED, &lv_font_montserrat_20);
    lv_label_set_long_mode(s_reader_uid, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_reader_uid, LV_PCT(100));
    lv_obj_set_style_text_align(s_reader_uid, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t* close = ui_make_button(card, "Close", &theme_style_btn_outline, reader_close_cb,
                                     nullptr);
    lv_obj_set_width(close, LV_PCT(100));

    ui_set_card_capture(on_reader_card);
}

static void reader_open_cb(lv_event_t*) { open_reader_modal(); }

static void build_debug(void) {
    lv_obj_clean(s_debug);

    lv_obj_t* card = ui_make_card(s_debug);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 8, 0);
    ui_make_label(card, "Debug", THEME_PRIMARY, &lv_font_montserrat_20);

    // Card reader — always available; harmless and handy for reading a UID.
    lv_obj_t* reader = ui_make_button(card, LV_SYMBOL_SD_CARD "  Read a card",
                                      &theme_style_btn_outline, reader_open_cb, nullptr);
    lv_obj_set_width(reader, LV_PCT(100));

    lv_obj_t* row = lv_obj_create(card);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    ui_make_label(row, "Enable debug tools", THEME_TEXT, &lv_font_montserrat_14);
    lv_obj_t* sw = lv_switch_create(row);
    lv_obj_add_event_cb(sw, debug_toggle_cb, LV_EVENT_VALUE_CHANGED, nullptr);

    s_wipe_btn = ui_make_button(card, LV_SYMBOL_TRASH "  Delete all cards & attendance",
                                &theme_style_btn_danger, wipe_cb, nullptr);
    lv_obj_set_width(s_wipe_btn, LV_PCT(100));
    lv_obj_add_flag(s_wipe_btn, LV_OBJ_FLAG_HIDDEN);  // revealed only when the toggle is on

    lv_obj_t* cap = ui_make_label(
        card, "Unbinds every student card and erases all attendance. Cannot be undone.",
        THEME_MUTED, &lv_font_montserrat_14);
    lv_label_set_long_mode(cap, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(cap, LV_PCT(100));

    s_card_wipe_btn = ui_make_button(card, LV_SYMBOL_TRASH "  Erase the whole SD card",
                                     &theme_style_btn_danger, card_wipe_cb, nullptr);
    lv_obj_set_width(s_card_wipe_btn, LV_PCT(100));
    lv_obj_add_flag(s_card_wipe_btn, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t* cap2 = ui_make_label(
        card,
        "Deletes every file on the card except config.json — including the students, "
        "classes, photos, exports and the face-detection models. Professors and their "
        "passwords survive. Cannot be undone.",
        THEME_MUTED, &lv_font_montserrat_14);
    lv_label_set_long_mode(cap2, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(cap2, LV_PCT(100));
}

// --- config import ----------------------------------------------------------

static void close_confirm(void) {
    if (s_confirm_modal) {
        lv_obj_delete(s_confirm_modal);
        s_confirm_modal = nullptr;
    }
    s_confirm_action = nullptr;
}

static void confirm_cancel_cb(lv_event_t*) { close_confirm(); }

static void confirm_ok_cb(lv_event_t*) {
    void (*act)(void) = s_confirm_action;
    close_confirm();
    if (act) act();
}

// A generic "Cancel / Confirm" overlay that runs `action` on confirm. Parented
// to the shell root and escaping its flex flow, like the wipe confirmation.
static void open_confirm(const char* title, const char* msg, void (*action)(void)) {
    if (s_confirm_modal) return;
    s_confirm_action = action;

    s_confirm_modal = lv_obj_create(s_root);
    lv_obj_remove_style_all(s_confirm_modal);
    lv_obj_add_flag(s_confirm_modal, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_set_size(s_confirm_modal, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_confirm_modal, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_confirm_modal, LV_OPA_50, 0);
    lv_obj_add_flag(s_confirm_modal, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(s_confirm_modal, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* card = ui_make_card(s_confirm_modal);
    lv_obj_set_width(card, LV_PCT(88));
    lv_obj_align(card, LV_ALIGN_TOP_MID, 0, 80);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 12, 0);

    ui_make_label(card, title, THEME_PRIMARY, &lv_font_montserrat_20);
    lv_obj_t* hint = ui_make_label(card, msg, THEME_MUTED, &lv_font_montserrat_14);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(hint, LV_PCT(100));

    lv_obj_t* row = lv_obj_create(card);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 10, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t* cancel =
        ui_make_button(row, "Cancel", &theme_style_btn_outline, confirm_cancel_cb, nullptr);
    lv_obj_set_flex_grow(cancel, 1);
    lv_obj_t* ok = ui_make_button(row, "Confirm", &theme_style_btn_primary, confirm_ok_cb, nullptr);
    lv_obj_set_flex_grow(ok, 1);
}

static void do_import(void) {
    import_result_t r = import_service_run(import_service_tar_path());
    ui_toast_show(r.message, r.ok);
    // Config/roster changed; refresh the sections that reflect them.
    build_import();
    build_storage();
    fill_profile();
}

static void do_revert(void) {
    import_result_t r = import_service_revert();
    ui_toast_show(r.message, r.ok);
    build_import();
    fill_profile();
}

static void import_btn_cb(lv_event_t*) {
    open_confirm("Import configuration?",
                 "This replaces the current configuration with the config.tar on the SD card. "
                 "The current configuration is backed up first — replacing the one backup "
                 "already kept, so only the configuration in use right now can be restored.",
                 do_import);
}

static void revert_btn_cb(lv_event_t*) {
    open_confirm("Restore previous configuration?",
                 "This re-applies the last backed-up configuration.", do_revert);
}

static void build_import(void) {
    lv_obj_clean(s_import);

    lv_obj_t* card = ui_make_card(s_import);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 8, 0);
    ui_make_label(card, "Configuration import", THEME_PRIMARY, &lv_font_montserrat_20);

    if (import_service_pending()) {
        lv_obj_t* m = ui_make_label(
            card,
            "A config.tar is on the SD card. Importing replaces the current configuration; the "
            "current one is backed up first. Only one backup is kept — importing again "
            "replaces it.",
            THEME_TEXT, &lv_font_montserrat_14);
        lv_label_set_long_mode(m, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(m, LV_PCT(100));
        lv_obj_t* btn = ui_make_button(card, LV_SYMBOL_DOWNLOAD "  Import configuration",
                                       &theme_style_btn_primary, import_btn_cb, nullptr);
        lv_obj_set_width(btn, LV_PCT(100));
    } else {
        lv_obj_t* m = ui_make_label(
            card,
            "To import, upload a config.tar to the SD card root (Wi-Fi file manager) or insert a "
            "card that has one, then reopen this panel.",
            THEME_MUTED, &lv_font_montserrat_14);
        lv_label_set_long_mode(m, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(m, LV_PCT(100));
    }

    if (backup_store_exists()) {
        lv_obj_t* rb = ui_make_button(card, LV_SYMBOL_LOOP "  Restore previous configuration",
                                      &theme_style_btn_outline, revert_btn_cb, nullptr);
        lv_obj_set_width(rb, LV_PCT(100));
    }
}

// --- screen -----------------------------------------------------------------

// Adds a rebuilt-on-show section container to the shell body.
static lv_obj_t* make_section(lv_obj_t* parent) {
    lv_obj_t* s = lv_obj_create(parent);
    lv_obj_remove_style_all(s);
    lv_obj_set_size(s, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s, LV_FLEX_FLOW_COLUMN);
    return s;
}

static lv_obj_t* create(void) {
    shell_t sh = shell_create("Admin panel", "Teacher profile", true);
    shell_set_active_nav(&sh, SCREEN_ADMIN);
    s_root = sh.root;

    // Profile (identity) card.
    lv_obj_t* profile = ui_make_card(sh.body);
    lv_obj_set_flex_flow(profile, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(profile, 4, 0);
    s_name = ui_make_label(profile, "", THEME_TEXT, &lv_font_montserrat_20);
    ui_label_fit(s_name);
    s_email = ui_make_label(profile, "", THEME_MUTED, &lv_font_montserrat_14);
    ui_label_fit(s_email);
    s_card = ui_make_label(profile, "", THEME_MUTED, &lv_font_montserrat_14);
    ui_label_fit(s_card);

    // SD-card usage first, then settings (camera preview), password, then debug
    // tools. Settings sits above password so the "Camera preview" button comes
    // before "Change password".
    s_storage = make_section(sh.body);
    s_settings = make_section(sh.body);
    s_security = make_section(sh.body);
    s_rfid = make_section(sh.body);
    s_import = make_section(sh.body);
    s_debug = make_section(sh.body);
    // Extra breathing room between the camera-preview button and the password
    // section that follows it.
    lv_obj_set_style_margin_top(s_security, 16, 0);

    // About: static content (credits/version), so it is built once here rather
    // than rebuilt on every show like the data-backed sections above.
    lv_obj_t* about_card = ui_make_card(sh.body);
    lv_obj_set_flex_flow(about_card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(about_card, 8, 0);
    ui_make_label(about_card, "About", THEME_PRIMARY, &lv_font_montserrat_20);
    lv_obj_t* about_btn = ui_make_button(about_card, LV_SYMBOL_FILE "  Project info & credits",
                                         &theme_style_btn_outline, about_cb, nullptr);
    lv_obj_set_width(about_btn, LV_PCT(100));
    ui_make_label(about_card, APP_VERSION_FULL, THEME_MUTED, &lv_font_montserrat_14);

    // Static sign-out button, built once (nothing to rebuild on show).
    lv_obj_t* sign_out = ui_make_button(sh.body, "Sign Out", &theme_style_btn_danger,
                                        sign_out_cb, nullptr);
    lv_obj_set_width(sign_out, LV_PCT(100));
    lv_obj_set_style_margin_top(sign_out, 24, 0);  // separate it from the sections above

    return sh.root;
}

static void on_show(void*) {
    fill_profile();
    build_storage();
    build_security();
    build_rfid();
    build_import();
    build_settings();
    build_debug();
}

static void on_hide(void) {
    close_pw_modal();
    close_rfid_modal();
    close_reader_modal();
    close_confirm();
    close_wipe_confirm();
    // Every overlay on this screen, without exception: they are parented to
    // s_root, so one left open survives the hide as a full-screen CLICKABLE
    // layer AND wedges its own opener (which returns early on a live pointer).
    close_card_wipe_confirm();
}

const screen_t scr_admin = {
    .create = create,
    .on_show = on_show,
    .on_hide = on_hide,
};
