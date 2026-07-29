#include "ui/screens/scr_kiosk.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "app/auth.h"
#include "app/roster.h"
#include "app/teacher.h"
#include "app/uid.h"
#include "audio/beeper.h"
#include "esp32-hal-log.h"
#include "esp_timer.h"
#include "services/face_detection_service.h"
#include "services/roster_service.h"
#include "storage/attendance_store.h"
#include "ui/components/face_verify.h"
#include "ui/components/keyboard.h"
#include "ui/components/status_bar.h"
#include "ui/components/student_photo.h"
#include "ui/components/toast.h"
#include "ui/screen_manager.h"
#include "ui/screens/scr_class.h"
#include "ui/theme/theme.h"
#include "ui/ui.h"

// Custom font carries the FontAwesome id-card glyph (see src/ui/assets/).
LV_FONT_DECLARE(font_montserrat_custom_32);
#define GLYPH_ID_CARD "\xEF\x8B\x82"  // U+F2C2 fa-users-id-card

static const char* TAG = "kiosk";

static constexpr uint32_t RESULT_MS = 5000;     // how long the confirmation shows
static constexpr uint32_t KIOSK_BG = 0xFFE8CC;  // light orange

static const class_rec_t* s_cls = nullptr;
static lv_obj_t* s_root = nullptr;
static lv_obj_t* s_header_title = nullptr;
static lv_obj_t* s_id_ta = nullptr;
static lv_obj_t* s_kbd = nullptr;
static lv_obj_t* s_result = nullptr;  // transient confirmation overlay
static lv_timer_t* s_timer = nullptr;
static lv_obj_t* s_exit_modal = nullptr;  // professor gate over the Exit button
static lv_obj_t* s_exit_ta = nullptr;

static void on_kiosk_card(const char* uid_hex);
static void on_exit_card(const char* uid_hex);
static void do_exit(void);

// Numeric-only keypad so the on-screen keyboard can stay up permanently
// without a mode-switch/close key that would let students type letters.
static const char* const KB_MAP[] = {"1", "2", "3", "\n", "4", "5", "6", "\n", "7", "8", "9",
                                     "\n", LV_SYMBOL_BACKSPACE, "0", LV_SYMBOL_OK, ""};
#define W1 ((lv_buttonmatrix_ctrl_t)1)  // relative width 1, no flags
static const lv_buttonmatrix_ctrl_t KB_CTRL[] = {W1, W1, W1, W1, W1, W1,
                                                 W1, W1, W1, W1, W1, W1};
#undef W1

// --- roster lookups ---------------------------------------------------------

static void digits_only(const char* s, char* out, size_t cap) {
    size_t j = 0;
    for (; *s && j + 1 < cap; s++) {
        if (isdigit((unsigned char)*s)) out[j++] = *s;
    }
    out[j] = '\0';
}

static const student_t* find_by_uid(const char* uid_hex) {
    char norm[32];
    uid_normalize(uid_hex, norm, sizeof(norm));
    if (!norm[0]) return nullptr;
    for (int j = 0; j < s_cls->roster_count; j++) {
        const student_t* st = roster_student_at(s_cls->roster[j]);
        if (!st || !st->rfid_uid[0]) continue;
        char sn[32];
        uid_normalize(st->rfid_uid, sn, sizeof(sn));
        if (strcmp(norm, sn) == 0) return st;
    }
    return nullptr;
}

// Matches on digits only, so "20230142" matches a "2023-0142" roster id.
static const student_t* find_by_id(const char* typed) {
    char t[24];
    digits_only(typed, t, sizeof(t));
    if (!t[0]) return nullptr;
    for (int j = 0; j < s_cls->roster_count; j++) {
        const student_t* st = roster_student_at(s_cls->roster[j]);
        if (!st) continue;
        char sd[24];
        digits_only(st->id, sd, sizeof(sd));
        if (sd[0] && strcmp(t, sd) == 0) return st;
    }
    return nullptr;
}

// --- confirmation overlay ---------------------------------------------------

static void dismiss_result(void) {
    if (s_timer) {
        lv_timer_delete(s_timer);
        s_timer = nullptr;
    }
    if (s_result) {
        lv_obj_delete(s_result);
        s_result = nullptr;
    }
}

static void result_timer_cb(lv_timer_t*) { dismiss_result(); }
static void result_click_cb(lv_event_t*) { dismiss_result(); }

// Full-screen colored panel on the top layer (covers the header, status bar
// and keyboard), tap- or timeout-dismissed.
static lv_obj_t* make_result(uint32_t bg) {
    dismiss_result();
    s_result = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_result);
    lv_obj_set_size(s_result, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_result, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_opa(s_result, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(s_result, 24, 0);
    lv_obj_set_flex_flow(s_result, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_result, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(s_result, 14, 0);
    lv_obj_add_flag(s_result, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_result, result_click_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_remove_flag(s_result, LV_OBJ_FLAG_SCROLLABLE);
    return s_result;
}

static void kiosk_label(lv_obj_t* parent, const char* text, const lv_font_t* font) {
    lv_obj_t* l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_color(l, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(l, LV_PCT(90));
}

// The student's avatar if it exists on the SD card, otherwise a placeholder
// circle — same photo/placeholder treatment as the in-class check-in overlay.
static void kiosk_photo(lv_obj_t* parent, const student_t* st) {
    char src[80];
    if (st && student_photo_src(st->id, src, sizeof(src))) {
        lv_obj_t* img = lv_image_create(parent);
        lv_image_set_src(img, src);
        return;
    }
    const int SZ = 140;
    lv_obj_t* ph = lv_obj_create(parent);
    lv_obj_remove_flag(ph, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(ph, SZ, SZ);
    lv_obj_set_style_radius(ph, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(ph, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(ph, LV_OPA_20, 0);
    lv_obj_set_style_border_width(ph, 0, 0);
    lv_obj_t* g = lv_label_create(ph);
    lv_label_set_text(g, LV_SYMBOL_IMAGE);
    lv_obj_set_style_text_color(g, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(g, &lv_font_montserrat_32, 0);
    lv_obj_center(g);
}

static void show_success(const student_t* st, bool already) {
    beeper_beep();
    lv_obj_t* p = make_result(THEME_SUCCESS);
    kiosk_photo(p, st);
    kiosk_label(p, already ? "Already checked in" : "Checked in", &lv_font_montserrat_32);
    kiosk_label(p, st->name, &lv_font_montserrat_32);
    char line[64];
    snprintf(line, sizeof(line), "ID %s", st->id);
    kiosk_label(p, line, &lv_font_montserrat_20);
    kiosk_label(p, attendance_date(), &lv_font_montserrat_20);
    s_timer = lv_timer_create(result_timer_cb, RESULT_MS, nullptr);
}

static void show_invalid(const char* typed) {
    beeper_error();
    lv_obj_t* p = make_result(THEME_DANGER);
    kiosk_label(p, LV_SYMBOL_CLOSE, &lv_font_montserrat_32);
    kiosk_label(p, "Not recognized", &lv_font_montserrat_32);
    char line[80];
    snprintf(line, sizeof(line), "\"%s\" is not in this class", typed);
    kiosk_label(p, line, &lv_font_montserrat_20);
    s_timer = lv_timer_create(result_timer_cb, RESULT_MS, nullptr);
}

// Timed-mode result panel: checked in (accent), present (green), or left early
// (red), each with the photo and the measured/running minutes.
static void show_timed_result(const student_t* st, att_state_t s) {
    uint32_t bg;
    const char* head;
    char sub[48];
    switch (s.status) {
        case ATT_PRESENT:
            bg = THEME_SUCCESS;
            head = "Present";
            snprintf(sub, sizeof(sub), "%d minutes", s.minutes);
            beeper_beep();
            break;
        case ATT_LEFT_EARLY:
            bg = THEME_DANGER;
            head = "Left early";
            snprintf(sub, sizeof(sub), "only %d minutes", s.minutes);
            beeper_error();
            break;
        case ATT_IN_PROGRESS:
        default:
            bg = THEME_ACCENT;
            head = "Checked in";
            snprintf(sub, sizeof(sub), "tap again when you leave");
            beeper_beep();
            break;
    }
    lv_obj_t* p = make_result(bg);
    kiosk_photo(p, st);
    kiosk_label(p, head, &lv_font_montserrat_32);
    kiosk_label(p, st->name, &lv_font_montserrat_32);
    kiosk_label(p, sub, &lv_font_montserrat_20);
    s_timer = lv_timer_create(result_timer_cb, RESULT_MS, nullptr);
}

// --- check-in ---------------------------------------------------------------

// Registers a recognized student and shows the result (timed or single).
static void kiosk_register(const student_t* st) {
    if (s_cls && s_cls->timed_attendance) {
        show_timed_result(st, attendance_tap(st->id, esp_timer_get_time(),
                                             s_cls->min_attendance_min));
        return;
    }
    bool already = attendance_is_present(st->id);
    if (!already) attendance_set(st->id, true);
    show_success(st, already);
}

static const student_t* s_verify_st = nullptr;  // student awaiting a face-verify

static void kiosk_verify_done(bool verified) {
    if (verified && s_verify_st) {
        kiosk_register(s_verify_st);
    } else {
        beeper_error();
        lv_obj_t* p = make_result(THEME_DANGER);
        kiosk_photo(p, s_verify_st);
        kiosk_label(p, "No face detected", &lv_font_montserrat_32);
        kiosk_label(p, "Please tap again", &lv_font_montserrat_20);
        s_timer = lv_timer_create(result_timer_cb, RESULT_MS, nullptr);
    }
    s_verify_st = nullptr;
    ui_set_card_capture(on_kiosk_card);  // re-arm for the next student
}

// Returns true if it started an async face-verify (the caller must NOT re-arm
// the card capture — kiosk_verify_done does that when verification finishes).
static bool check_in(const student_t* st) {
    if (s_id_ta) lv_textarea_set_text(s_id_ta, "");  // clear the field on success
    if (s_cls && class_capture_enabled(s_cls)) {
        // Loud, because "nothing happened and nothing was logged" is exactly how
        // this feature fails in the field: photo check-in is a PER-CLASS flag and
        // an imported config.tar resets it to false (CONFIG_IMPORT.md §3.3).
        ESP_LOGI(TAG, "check-in %s: capture enabled, opening face verify", st->id);
        if (!face_detection_running()) {
            beeper_error();
            lv_obj_t* p = make_result(THEME_DANGER);
            kiosk_label(p, LV_SYMBOL_WARNING, &lv_font_montserrat_32);
            kiosk_label(p, "Camera unavailable", &lv_font_montserrat_32);
            s_timer = lv_timer_create(result_timer_cb, RESULT_MS, nullptr);
            return false;
        }
        s_verify_st = st;
        ui_set_card_capture(nullptr);  // no stray taps while the overlay is up
        face_verify_open(st->id, st->name, attendance_date(), s_cls->code,
                         s_cls->face_verify_seconds, kiosk_verify_done);
        return true;
    }
    ESP_LOGI(TAG, "check-in %s: photo capture OFF for class %s — no camera step",
             st->id, s_cls ? s_cls->code : "?");
    kiosk_register(st);
    return false;
}

static void on_kiosk_card(const char* uid_hex) {
    // A professor card is the way out: tapping it anywhere in kiosk leaves
    // immediately, without going through the Exit button + gate modal. Checked
    // BEFORE the student lookup so a professor who is also enrolled as a student
    // still exits rather than checking themselves in. Students can't abuse this
    // — the gate is the card itself, exactly as in the exit modal.
    teacher_t prof;
    if (auth_lookup_uid(uid_hex, &prof)) {
        do_exit();
        return;  // do_exit() navigates away; on_hide() disarms the capture
    }

    const student_t* st = find_by_uid(uid_hex);
    bool async = false;
    if (st) {
        async = check_in(st);
    } else {
        show_invalid("card");
    }
    if (!async) ui_set_card_capture(on_kiosk_card);  // stay armed (verify re-arms itself)
}

static void do_submit(void) {
    const char* typed = s_id_ta ? lv_textarea_get_text(s_id_ta) : "";
    if (!typed[0]) return;
    const student_t* st = find_by_id(typed);
    if (st) {
        check_in(st);  // card capture is managed inside (disarmed during a verify)
    } else {
        show_invalid(typed);
    }
}

static void submit_cb(lv_event_t*) { do_submit(); }
static void kb_ready_cb(lv_event_t*) { do_submit(); }        // keypad OK checks in
static void kb_key_beep_cb(lv_event_t*) { beeper_touch(); }  // tick on each keypress

// --- exit gate (professor card or password) ---------------------------------
//
// Kiosk is unattended, so a student mustn't be able to leave it. There are two
// ways out, both requiring professor credentials:
//   1. Tap a professor card at any time — handled in on_kiosk_card(), exits
//      straight away (the common case: the professor is standing right there).
//   2. Tap the Exit button, which opens this modal; a professor card tap or a
//      professor password closes it. This is the fallback for a professor who
//      has no card bound, or whose card isn't to hand.

static void close_exit_modal(void) {
    if (s_exit_modal) {
        keyboard_hide();
        lv_obj_delete(s_exit_modal);
        s_exit_modal = nullptr;
        s_exit_ta = nullptr;
    }
}

static void do_exit(void) {
    close_exit_modal();
    beeper_beep();
    scr_class_request_session_view();  // return into the running session, not the hub
    scr_mgr_show(SCREEN_CLASS, (void*)const_cast<class_rec_t*>(s_cls));
}

// A professor card tap in the exit modal finishes kiosk mode.
static void on_exit_card(const char* uid_hex) {
    teacher_t who;
    if (auth_lookup_uid(uid_hex, &who)) {
        do_exit();
    } else {
        beeper_error();
        ui_toast_show("Not a professor card", false);
        ui_set_card_capture(on_exit_card);  // stay armed for another try
    }
}

static void exit_pw_ok_cb(lv_event_t*) {
    const char* pw = s_exit_ta ? lv_textarea_get_text(s_exit_ta) : "";
    teacher_t who;
    if (auth_lookup_password(pw, &who)) {
        do_exit();
    } else {
        beeper_error();
        ui_toast_show("Wrong password", false);
    }
}

static void exit_cancel_cb(lv_event_t*) {
    close_exit_modal();
    ui_set_card_capture(on_kiosk_card);  // back to student check-in
}

static void open_exit_modal(void) {
    if (s_exit_modal) return;
    dismiss_result();  // clear any confirmation overlay first

    // Parented to s_root (not lv_layer_top) so the shared password keyboard,
    // which lives on lv_layer_top, still floats above this modal.
    s_exit_modal = lv_obj_create(s_root);
    lv_obj_remove_style_all(s_exit_modal);
    // Escape the screen's flex flow so this is a true full-screen overlay.
    lv_obj_add_flag(s_exit_modal, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_set_size(s_exit_modal, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_exit_modal, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_exit_modal, LV_OPA_50, 0);
    lv_obj_add_flag(s_exit_modal, LV_OBJ_FLAG_CLICKABLE);  // swallow taps behind
    lv_obj_remove_flag(s_exit_modal, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* card = ui_make_card(s_exit_modal);
    lv_obj_set_width(card, LV_PCT(88));
    lv_obj_align(card, LV_ALIGN_TOP_MID, 0, 60);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 12, 0);

    ui_make_label(card, "Exit kiosk?", THEME_PRIMARY, &lv_font_montserrat_20);
    lv_obj_t* hint = ui_make_label(card, "Tap a professor card, or enter a password.",
                                   THEME_MUTED, &lv_font_montserrat_14);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(hint, LV_PCT(100));

    // Passwords are digits-only: use the shared numeric keypad (same as the
    // idle login), pop it up right away, and make its OK/finish button submit.
    s_exit_ta = keyboard_make_textarea(card, "Password", 32, LV_KEYBOARD_MODE_NUMBER);
    lv_textarea_set_password_mode(s_exit_ta, true);
    keyboard_show(s_exit_ta, LV_KEYBOARD_MODE_NUMBER);
    keyboard_set_ready_cb(exit_pw_ok_cb);

    lv_obj_t* row = lv_obj_create(card);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 10, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t* cancel = ui_make_button(row, "Cancel", &theme_style_btn_outline,
                                      exit_cancel_cb, nullptr);
    lv_obj_set_flex_grow(cancel, 1);
    lv_obj_t* ok = ui_make_button(row, "Exit", &theme_style_btn_primary, exit_pw_ok_cb,
                                  nullptr);
    lv_obj_set_flex_grow(ok, 1);

    ui_set_card_capture(on_exit_card);  // divert the next card tap to the gate
}

static void exit_cb(lv_event_t*) { open_exit_modal(); }

// --- screen -----------------------------------------------------------------

static lv_obj_t* create(void) {
    s_root = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_root, lv_color_hex(KIOSK_BG), 0);
    lv_obj_set_style_pad_all(s_root, 0, 0);
    lv_obj_set_style_pad_top(s_root, STATUS_BAR_HEIGHT, 0);
    lv_obj_remove_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(s_root, LV_FLEX_FLOW_COLUMN);

    // Header: class name + subtitle, with an exit button.
    lv_obj_t* header = lv_obj_create(s_root);
    lv_obj_remove_style_all(header);
    lv_obj_set_size(header, LV_PCT(100), 64);
    lv_obj_set_style_bg_color(header, lv_color_hex(THEME_PRIMARY), 0);
    lv_obj_set_style_bg_grad_color(header, lv_color_hex(THEME_PRIMARY_DARK), 0);
    lv_obj_set_style_bg_grad_dir(header, LV_GRAD_DIR_HOR, 0);
    lv_obj_set_style_bg_opa(header, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(header, 14, 0);
    lv_obj_set_style_pad_column(header, 10, 0);
    lv_obj_remove_flag(header, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    lv_obj_t* tcol = lv_obj_create(header);
    lv_obj_remove_style_all(tcol);
    lv_obj_set_height(tcol, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(tcol, 1);
    lv_obj_set_flex_flow(tcol, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(tcol, LV_OBJ_FLAG_SCROLLABLE);
    s_header_title = ui_make_label(tcol, "", THEME_ON_PRIMARY, &lv_font_montserrat_20);
    lv_label_set_long_mode(s_header_title, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_header_title, LV_PCT(100));
    ui_make_label(tcol, "Student check-in", THEME_HEADER_SUBTITLE, &lv_font_montserrat_20);

    lv_obj_t* exit = lv_button_create(header);
    lv_obj_remove_style_all(exit);
    lv_obj_set_size(exit, 44, 44);
    lv_obj_set_style_radius(exit, 10, 0);
    lv_obj_set_style_bg_color(exit, lv_color_hex(THEME_ON_PRIMARY), 0);
    lv_obj_set_style_bg_opa(exit, LV_OPA_20, 0);
    lv_obj_add_event_cb(exit, exit_cb, LV_EVENT_CLICKED, nullptr);
    ui_add_press_feedback(exit);
    lv_obj_center(ui_make_label(exit, LV_SYMBOL_CLOSE, THEME_ON_PRIMARY, &lv_font_montserrat_20));

    // Body: entry card centered between the header and the keypad.
    lv_obj_t* body = lv_obj_create(s_root);
    lv_obj_remove_style_all(body);
    lv_obj_set_width(body, LV_PCT(100));
    lv_obj_set_flex_grow(body, 1);
    lv_obj_set_style_pad_all(body, 16, 0);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(body, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(body, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* card = ui_make_card(body);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(card, 12, 0);
    lv_obj_set_style_pad_ver(card, 18, 0);

    ui_make_label(card, GLYPH_ID_CARD, THEME_ACCENT, &font_montserrat_custom_32);
    ui_make_label(card, "Tap your card to check in", THEME_PRIMARY, &lv_font_montserrat_32);
    ui_make_label(card, "or type your ID and press OK", THEME_MUTED, &lv_font_montserrat_20);

    s_id_ta = lv_textarea_create(card);
    lv_obj_remove_style_all(s_id_ta);
    lv_obj_add_style(s_id_ta, &theme_style_input, 0);
    lv_obj_set_style_text_color(s_id_ta, lv_color_hex(THEME_MUTED), LV_PART_TEXTAREA_PLACEHOLDER);
    lv_obj_set_style_text_font(s_id_ta, &lv_font_montserrat_20, 0);
    lv_obj_set_width(s_id_ta, LV_PCT(100));
    lv_textarea_set_one_line(s_id_ta, true);
    lv_textarea_set_max_length(s_id_ta, 15);
    lv_textarea_set_placeholder_text(s_id_ta, "University ID");

    lv_obj_t* submit = ui_make_button(card, "Check in", &theme_style_btn_primary, submit_cb,
                                      nullptr);
    lv_obj_set_width(submit, LV_PCT(100));
    lv_obj_set_style_text_font(lv_obj_get_child(submit, 0), &lv_font_montserrat_20, 0);

    // Always-visible numeric keypad, wired to the ID field.
    s_kbd = lv_keyboard_create(s_root);
    lv_keyboard_set_mode(s_kbd, LV_KEYBOARD_MODE_NUMBER);
    lv_keyboard_set_map(s_kbd, LV_KEYBOARD_MODE_NUMBER, KB_MAP, KB_CTRL);
    lv_keyboard_set_textarea(s_kbd, s_id_ta);
    lv_obj_set_height(s_kbd, 300);
    lv_obj_set_style_text_font(s_kbd, &lv_font_montserrat_20, 0);
    lv_obj_add_event_cb(s_kbd, kb_ready_cb, LV_EVENT_READY, nullptr);
    lv_obj_add_event_cb(s_kbd, kb_key_beep_cb, LV_EVENT_VALUE_CHANGED, nullptr);

    return s_root;
}

static void on_show(void* arg) {
    if (arg) s_cls = (const class_rec_t*)arg;
    lv_label_set_text(s_header_title, s_cls ? s_cls->name : "Kiosk");
    if (s_id_ta) lv_textarea_set_text(s_id_ta, "");
    keyboard_hide();  // the shared keyboard from other screens; kiosk uses its own
    dismiss_result();
    close_exit_modal();
    if (s_cls && class_capture_enabled(s_cls)) face_detection_start();  // warm for verify
    ui_set_card_capture(on_kiosk_card);                                 // students tap to check in
}

static void on_hide(void) {
    ui_set_card_capture(nullptr);
    dismiss_result();
    close_exit_modal();
    face_verify_cancel();
    face_detection_stop();
}

const screen_t scr_kiosk = {
    .create = create,
    .on_show = on_show,
    .on_hide = on_hide,
};
