#include "ui/screens/scr_class.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "app/auth.h"
#include "app/uid.h"
#include "audio/beeper.h"
#include "services/roster_service.h"
#include "storage/attendance_store.h"
#include "ui/components/keyboard.h"
#include "ui/components/shell.h"
#include "ui/components/toast.h"
#include "ui/screen_manager.h"
#include "ui/theme/theme.h"
#include "ui/ui.h"

// FontAwesome glyphs merged into the custom Montserrat font (see
// src/ui/assets/): lock, unlock, and users-viewfinder (kiosk). Used as UTF-8.
LV_FONT_DECLARE(font_montserrat_custom_20);
#define GLYPH_LOCK "\xEF\x80\xA3"     // U+F023 fa-lock
#define GLYPH_UNLOCK "\xEF\x82\x9C"   // U+F09C fa-unlock
#define GLYPH_KIOSK "\xEE\x96\x95"    // U+E595 fa-users-viewfinder
#define GLYPH_ID_CARD "\xEF\x8B\x82"  // U+F2C2 fa-users-id-card

typedef enum { TAB_SESSION,
               TAB_HISTORY,
               TAB_ENROLL,
               TAB_COUNT } tab_id_t;
typedef enum { ENROLL_SEARCH,
               ENROLL_MANUAL,
               ENROLL_WAIT } enroll_state_t;

static const char* TAB_NAMES[TAB_COUNT] = {"Session", "History", "Enroll"};

static constexpr int MAX_DATES = 24;  // recent/history dates listed

static shell_t s_sh;
static lv_obj_t* s_tab_btns[TAB_COUNT];
static lv_obj_t* s_content = nullptr;  // rebuilt on tab switch

static const class_rec_t* s_cls = nullptr;
static tab_id_t s_tab = TAB_SESSION;

// Session/date-picker state.
static char s_sel_date[12] = "";     // date chosen in the calendar
static char s_dates[MAX_DATES][12];  // listed past dates
static int s_dates_count = 0;
static lv_calendar_date_t s_highlight;  // must outlive the calendar

// Enroll sub-flow state.
static enroll_state_t s_enroll_state = ENROLL_SEARCH;
static bool s_unregistered_only = true;  // toggle: only students without a card
static lv_obj_t* s_search_ta = nullptr;
static lv_obj_t* s_results = nullptr;
static lv_obj_t* s_manual_id_ta = nullptr;
static lv_obj_t* s_manual_name_ta = nullptr;
static int s_sel_idx = -1;         // selected existing student (registry index)
static bool s_sel_is_new = false;  // selecting a manually-entered new student
static char s_new_id[20];
static char s_new_name[48];

// Enrollment lock: the Enroll tab is gated behind a professor card/password.
static bool s_unlocked = false;
static lv_obj_t* s_lock_btn = nullptr;
static lv_obj_t* s_kiosk_btn = nullptr;
static lv_obj_t* s_unlock_modal = nullptr;
static lv_obj_t* s_unlock_ta = nullptr;
static bool s_unlock_goto_enroll = false;  // jump to Enroll after a successful unlock

static void rebuild_content(void);
static void enroll_goto(enroll_state_t st);
static void update_results(void);
static void on_enroll_card(const char* uid_hex);
static void build_session(void);
static void build_session_open(void);
static void on_session_card(const char* uid_hex);
static void update_lock_state(void);
static void open_unlock_modal(void);
static void on_unlock_card(const char* uid_hex);

static void back_cb(lv_event_t*) { scr_mgr_show(SCREEN_CLASSES, nullptr); }

// --- Session tab: date picker + roll call -----------------------------------

// registry index of the roster student whose card matches, or -1.
static int roster_student_by_uid(const char* uid_hex) {
    char norm[32];
    uid_normalize(uid_hex, norm, sizeof(norm));
    if (!norm[0]) return -1;
    for (int j = 0; j < s_cls->roster_count; j++) {
        int idx = s_cls->roster[j];
        const student_t* st = roster_student_at(idx);
        if (!st || !st->rfid_uid[0]) continue;
        char sn[32];
        uid_normalize(st->rfid_uid, sn, sizeof(sn));
        if (strcmp(norm, sn) == 0) return idx;
    }
    return -1;
}

static void rebuild_session_open(void) {
    lv_obj_clean(s_content);
    build_session_open();
}

// A card tap during an open session marks that student present.
static void on_session_card(const char* uid_hex) {
    int idx = roster_student_by_uid(uid_hex);
    if (idx < 0) {
        beeper_error();
        ui_toast_show("Card not recognized in this class", false);
    } else {
        beeper_beep();
        attendance_set(roster_student_at(idx)->id, true);
    }
    rebuild_session_open();  // refresh + re-arm the card capture
}

static void roll_toggle_cb(lv_event_t* e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    const student_t* st = roster_student_at(idx);
    if (!st) return;
    attendance_set(st->id, !attendance_is_present(st->id));
    rebuild_session_open();
}

static void close_session_cb(lv_event_t*) {
    attendance_close();
    rebuild_content();  // back to the picker
}

// Orders two registry indices alphabetically by student name (case-insensitive)
// for the roll-call list.
static int roster_name_cmp(const void* a, const void* b) {
    const student_t* sa = roster_student_at(*(const int*)a);
    const student_t* sb = roster_student_at(*(const int*)b);
    return strcasecmp(sa ? sa->name : "", sb ? sb->name : "");
}

static void build_session_open(void) {
    // Instructional callout — deliberately NOT button-shaped: soft orange fill
    // with an amber left rule and alert icon so it reads as a notice, not a tap
    // target. Icon + text sit in a left-aligned row.
    lv_obj_t* banner = ui_make_card(s_content);
    lv_obj_set_style_bg_color(banner, lv_color_hex(THEME_WARNING_SOFT), 0);
    lv_obj_set_style_border_color(banner, lv_color_hex(THEME_WARNING), 0);
    lv_obj_set_style_border_width(banner, 5, 0);
    lv_obj_set_style_border_side(banner, LV_BORDER_SIDE_LEFT, 0);
    lv_obj_set_flex_flow(banner, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(banner, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(banner, 12, 0);
    ui_make_label(banner, LV_SYMBOL_WARNING, THEME_WARNING, &lv_font_montserrat_20);
    ui_make_label(banner, "Class open: Tap a card, or tap a name to log it.", THEME_TEXT, &lv_font_montserrat_14);

    int total = s_cls->roster_count;
    int present = attendance_present_count();

    lv_obj_t* summary = ui_make_card(s_content);
    lv_obj_set_flex_flow(summary, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(summary, 8, 0);

    lv_obj_t* head = lv_obj_create(summary);
    lv_obj_remove_style_all(head);
    lv_obj_set_size(head, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    ui_make_label(head, attendance_date(), THEME_PRIMARY, &lv_font_montserrat_20);
    char count_txt[32];
    snprintf(count_txt, sizeof(count_txt), "%d/%d present", present, total);
    ui_make_label(head, count_txt, THEME_SUCCESS, &lv_font_montserrat_14);

    lv_obj_t* bar = lv_bar_create(summary);
    lv_obj_set_size(bar, LV_PCT(100), 8);
    lv_bar_set_range(bar, 0, total > 0 ? total : 1);
    lv_bar_set_value(bar, present, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bar, lv_color_hex(THEME_BORDER), LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, lv_color_hex(THEME_SUCCESS), LV_PART_INDICATOR);

    lv_obj_t* close = ui_make_button(summary, "Close session", &theme_style_btn_outline,
                                     close_session_cb, nullptr);
    lv_obj_set_width(close, LV_PCT(100));

    if (total == 0) {
        lv_obj_t* card = ui_make_card(s_content);
        lv_obj_center(ui_make_label(card, "No students enrolled in this class yet.",
                                    THEME_MUTED, &lv_font_montserrat_14));
    }

    // Render the roll call alphabetically by name, not roster/registry order.
    int order[ROSTER_MAX_CLASS_STUDENTS];
    for (int j = 0; j < total; j++) order[j] = s_cls->roster[j];
    qsort(order, total, sizeof(order[0]), roster_name_cmp);

    for (int j = 0; j < total; j++) {
        int idx = order[j];
        const student_t* st = roster_student_at(idx);
        if (!st) continue;
        bool here = attendance_is_present(st->id);

        lv_obj_t* chip = ui_make_card(s_content);
        lv_obj_set_style_bg_color(chip, lv_color_hex(here ? THEME_SUCCESS_SOFT : THEME_SURFACE),
                                  0);
        lv_obj_set_style_border_color(chip, lv_color_hex(here ? THEME_SUCCESS : THEME_BORDER),
                                      0);
        lv_obj_set_flex_flow(chip, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(chip, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_add_flag(chip, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(chip, roll_toggle_cb, LV_EVENT_CLICKED, (void*)(intptr_t)idx);
        ui_add_press_feedback(chip);

        lv_obj_t* col = lv_obj_create(chip);
        lv_obj_remove_style_all(col);
        lv_obj_set_size(col, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(col, 2, 0);
        ui_make_label(col, st->name, THEME_TEXT, &lv_font_montserrat_14);
        ui_make_label(col, st->id, here ? THEME_SUCCESS : THEME_MUTED, &lv_font_montserrat_14);

        ui_make_label(chip, here ? LV_SYMBOL_OK : "", here ? THEME_SUCCESS : THEME_MUTED,
                      &lv_font_montserrat_20);
    }

    ui_set_card_capture(on_session_card);
}

static void cal_changed_cb(lv_event_t* e) {
    // The day-click event bubbles up from the calendar's internal button
    // matrix, so get_target() would be that matrix; get_current_target() is
    // the calendar the handler is bound to (required by get_pressed_date).
    lv_obj_t* cal = (lv_obj_t*)lv_event_get_current_target(e);
    lv_calendar_date_t d;
    if (lv_calendar_get_pressed_date(cal, &d) == LV_RESULT_OK) {
        snprintf(s_sel_date, sizeof(s_sel_date), "%04d-%02d-%02d", d.year, d.month, d.day);
        // Move the highlight box to the tapped day so the selection is visible.
        s_highlight = d;
        lv_calendar_set_highlighted_dates(cal, &s_highlight, 1);
    }
}

static void open_selected_cb(lv_event_t*) {
    if (!s_sel_date[0]) return;
    attendance_open(s_cls->dir, s_sel_date);
    rebuild_content();  // -> open roll call
}

// Opens one of the listed dates (recent list or history) and shows it.
static void open_date_cb(lv_event_t* e) {
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= s_dates_count) return;
    attendance_open(s_cls->dir, s_dates[i]);
    s_tab = TAB_SESSION;
    rebuild_content();
}

// One tappable "date — N/total present" row. With show_pct, the attendance
// percentage for that session is appended (used on the History tab).
static void add_date_row(lv_obj_t* parent, int i, bool show_pct) {
    int present = attendance_present_for(s_cls->dir, s_dates[i]);
    int total = s_cls->roster_count;

    lv_obj_t* row = ui_make_card(parent);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, open_date_cb, LV_EVENT_CLICKED, (void*)(intptr_t)i);
    ui_add_press_feedback(row);

    ui_make_label(row, s_dates[i], THEME_TEXT, &lv_font_montserrat_14);
    char sub[40];
    if (show_pct) {
        int pct = total > 0 ? (present * 100 + total / 2) / total : 0;
        snprintf(sub, sizeof(sub), "%d/%d present  %d%%", present, total, pct);
    } else {
        snprintf(sub, sizeof(sub), "%d/%d present", present, total);
    }
    ui_make_label(row, sub, THEME_MUTED, &lv_font_montserrat_14);
}

static void build_session_closed(void) {
    s_dates_count = attendance_list_dates(s_cls->dir, s_dates, MAX_DATES);

    // Placeholder = last session date, so the picker opens on the right month.
    int y = 2026, m = 1, d = 1;
    if (s_dates_count > 0) sscanf(s_dates[0], "%d-%d-%d", &y, &m, &d);
    snprintf(s_sel_date, sizeof(s_sel_date), "%04d-%02d-%02d", y, m, d);

    lv_obj_t* card = ui_make_card(s_content);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 10, 0);

    ui_make_label(card, "Open a session", THEME_PRIMARY, &lv_font_montserrat_20);
    lv_obj_t* hint = ui_make_label(
        card, "Pick the class date (no clock on the device), then open the session.",
        THEME_MUTED, &lv_font_montserrat_14);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(hint, LV_PCT(100));

    lv_obj_t* cal = lv_calendar_create(card);
    lv_obj_set_size(cal, LV_PCT(100), 300);
    lv_calendar_set_showed_date(cal, y, m);
    // Highlight = the current selection (placeholder at first, follows taps).
    s_highlight.year = y;
    s_highlight.month = m;
    s_highlight.day = d;
    lv_calendar_set_highlighted_dates(cal, &s_highlight, 1);
    lv_calendar_header_dropdown_create(cal);
    lv_obj_add_event_cb(cal, cal_changed_cb, LV_EVENT_VALUE_CHANGED, nullptr);

    lv_obj_t* open = ui_make_button(card, "Open session", &theme_style_btn_primary,
                                    open_selected_cb, nullptr);
    lv_obj_set_width(open, LV_PCT(100));

    if (s_dates_count > 0) {
        lv_obj_t* rc = lv_obj_create(s_content);
        lv_obj_remove_style_all(rc);
        lv_obj_set_size(rc, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(rc, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(rc, 8, 0);
        ui_make_label(rc, "Recent sessions", THEME_PRIMARY, &lv_font_montserrat_20);
        for (int i = 0; i < s_dates_count && i < 5; i++) add_date_row(rc, i, false);
    }
}

// True if an attendance session is open for the class being shown.
static bool session_open_here(void) {
    return attendance_is_open() && strcmp(attendance_dir(), s_cls->dir) == 0;
}

static void build_session(void) {
    if (session_open_here()) {
        build_session_open();
    } else {
        build_session_closed();
    }
}

// --- History tab ------------------------------------------------------------

static void build_history(void) {
    s_dates_count = attendance_list_dates(s_cls->dir, s_dates, MAX_DATES);

    if (s_dates_count == 0) {
        lv_obj_t* card = ui_make_card(s_content);
        lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(card, 6, 0);
        ui_make_label(card, "No attendance records yet", THEME_TEXT, &lv_font_montserrat_14);
        lv_obj_t* msg = ui_make_label(
            card, "Open a session in the Session tab to record attendance.", THEME_MUTED,
            &lv_font_montserrat_14);
        lv_label_set_long_mode(msg, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(msg, LV_PCT(100));
        return;
    }

    ui_make_label(s_content, "Past sessions (tap to edit)", THEME_PRIMARY,
                  &lv_font_montserrat_20);
    for (int i = 0; i < s_dates_count; i++) add_date_row(s_content, i, true);
}

// --- Enroll: search ---------------------------------------------------------

// Case-insensitive substring test.
static bool istr_has(const char* hay, const char* needle) {
    if (!needle[0]) return true;
    for (const char* h = hay; *h; h++) {
        const char* a = h;
        const char* b = needle;
        while (*a && *b && tolower((unsigned char)*a) == tolower((unsigned char)*b)) {
            a++;
            b++;
        }
        if (!*b) return true;
    }
    return false;
}

static void select_existing_cb(lv_event_t* e) {
    s_sel_idx = (int)(intptr_t)lv_event_get_user_data(e);
    s_sel_is_new = false;
    enroll_goto(ENROLL_WAIT);
}

static void add_manual_cb(lv_event_t*) {
    // Prefill the name with the search text (usually a name that wasn't found).
    const char* q = s_search_ta ? lv_textarea_get_text(s_search_ta) : "";
    snprintf(s_new_name, sizeof(s_new_name), "%s", q);
    s_new_id[0] = '\0';
    enroll_goto(ENROLL_MANUAL);
}

// Rebuilds the autocomplete list from the current query + toggle. Only the
// students enrolled in this class (class.json's roster) are searched.
static void update_results(void) {
    lv_obj_clean(s_results);
    const char* q = s_search_ta ? lv_textarea_get_text(s_search_ta) : "";

    int shown = 0;
    for (int j = 0; j < s_cls->roster_count && shown < 6; j++) {
        int idx = s_cls->roster[j];  // index into the global registry
        const student_t* st = roster_student_at(idx);
        if (!st) continue;
        if (s_unregistered_only && st->rfid_uid[0]) continue;
        if (!istr_has(st->name, q) && !istr_has(st->id, q)) continue;

        lv_obj_t* row = ui_make_card(s_results);
        lv_obj_set_style_pad_all(row, 10, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(row, 2, 0);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, select_existing_cb, LV_EVENT_CLICKED, (void*)(intptr_t)idx);
        ui_add_press_feedback(row);

        ui_make_label(row, st->name, THEME_TEXT, &lv_font_montserrat_14);
        char sub[72];
        snprintf(sub, sizeof(sub), "%s%s", st->id, st->rfid_uid[0] ? "   card registered" : "");
        ui_make_label(row, sub, st->rfid_uid[0] ? THEME_SUCCESS : THEME_MUTED,
                      &lv_font_montserrat_14);
        shown++;
    }

    if (shown == 0) {
        lv_obj_t* card = ui_make_card(s_results);
        lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(card, 8, 0);
        ui_make_label(card, q[0] ? "No matching student" : "Type a name or ID to search",
                      THEME_MUTED, &lv_font_montserrat_14);
        if (q[0]) {
            lv_obj_t* add = ui_make_button(card, LV_SYMBOL_PLUS "  Add new student",
                                           &theme_style_btn_outline, add_manual_cb, nullptr);
            lv_obj_set_width(add, LV_PCT(100));
        }
    }
}

static void toggle_cb(lv_event_t* e) {
    lv_obj_t* sw = (lv_obj_t*)lv_event_get_target(e);
    s_unregistered_only = lv_obj_has_state(sw, LV_STATE_CHECKED);
    update_results();
}

static void search_changed_cb(lv_event_t*) { update_results(); }

static void build_enroll_search(void) {
    lv_obj_t* card = ui_make_card(s_content);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 10, 0);

    lv_obj_t* trow = lv_obj_create(card);
    lv_obj_remove_style_all(trow);
    lv_obj_set_size(trow, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(trow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(trow, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(trow, 10, 0);
    lv_obj_t* tlabel = ui_make_label(trow, "Only students without a card", THEME_TEXT,
                                     &lv_font_montserrat_14);
    lv_obj_set_flex_grow(tlabel, 1);
    lv_label_set_long_mode(tlabel, LV_LABEL_LONG_WRAP);
    lv_obj_t* sw = lv_switch_create(trow);
    if (s_unregistered_only) lv_obj_add_state(sw, LV_STATE_CHECKED);
    lv_obj_add_event_cb(sw, toggle_cb, LV_EVENT_VALUE_CHANGED, nullptr);

    s_search_ta = keyboard_make_textarea(card, "Search by name or ID", 47,
                                         LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_obj_add_event_cb(s_search_ta, search_changed_cb, LV_EVENT_VALUE_CHANGED, nullptr);

    s_results = lv_obj_create(s_content);
    lv_obj_remove_style_all(s_results);
    lv_obj_set_size(s_results, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_results, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_results, 8, 0);
    update_results();
}

// --- Enroll: manual entry ---------------------------------------------------

static void manual_back_cb(lv_event_t*) { enroll_goto(ENROLL_SEARCH); }

static void manual_continue_cb(lv_event_t*) {
    keyboard_hide();
    const char* id = lv_textarea_get_text(s_manual_id_ta);
    const char* name = lv_textarea_get_text(s_manual_name_ta);
    if (!id[0] || !name[0]) {
        ui_toast_show("Enter both name and ID", false);
        return;
    }
    snprintf(s_new_id, sizeof(s_new_id), "%s", id);
    snprintf(s_new_name, sizeof(s_new_name), "%s", name);
    s_sel_is_new = true;
    enroll_goto(ENROLL_WAIT);
}

static void build_enroll_manual(void) {
    lv_obj_t* card = ui_make_card(s_content);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 8, 0);

    ui_make_label(card, "Add new student", THEME_PRIMARY, &lv_font_montserrat_20);

    ui_make_label(card, "Student ID", THEME_TEXT, &lv_font_montserrat_14);
    s_manual_id_ta = keyboard_make_textarea(card, "e.g. 2024-0123", 15,
                                            LV_KEYBOARD_MODE_NUMBER);
    if (s_new_id[0]) lv_textarea_set_text(s_manual_id_ta, s_new_id);

    ui_make_label(card, "Full name", THEME_TEXT, &lv_font_montserrat_14);
    s_manual_name_ta = keyboard_make_textarea(card, "Student name", 47,
                                              LV_KEYBOARD_MODE_TEXT_LOWER);
    if (s_new_name[0]) lv_textarea_set_text(s_manual_name_ta, s_new_name);

    lv_obj_t* cont = ui_make_button(card, "Continue to card", &theme_style_btn_primary,
                                    manual_continue_cb, nullptr);
    lv_obj_set_width(cont, LV_PCT(100));
    lv_obj_t* back = ui_make_button(card, "Back to search", &theme_style_btn_outline,
                                    manual_back_cb, nullptr);
    lv_obj_set_width(back, LV_PCT(100));
}

// --- Enroll: wait for card --------------------------------------------------

static void wait_cancel_cb(lv_event_t*) { enroll_goto(ENROLL_SEARCH); }

// Runs on the LVGL thread (via the card-capture path) when a card is tapped.
static void on_enroll_card(const char* uid_hex) {
    roster_result_t r;
    const char* sid;
    if (s_sel_is_new) {
        r = roster_enroll_new(s_cls->code, s_new_id, s_new_name, uid_hex);
        sid = s_new_id;
    } else {
        r = roster_enroll_existing(s_cls->code, s_sel_idx, uid_hex);
        const student_t* st = roster_student_at(s_sel_idx);
        sid = st ? st->id : nullptr;
    }
    if (r.ok) {
        beeper_beep();
        // Enrolling always happens during an open session: check the student
        // in right away.
        char msg[128];
        if (sid && session_open_here()) {
            attendance_set(sid, true);
            snprintf(msg, sizeof(msg), "%s - checked in", r.message);
        } else {
            snprintf(msg, sizeof(msg), "%s", r.message);
        }
        ui_toast_show(msg, true);
        enroll_goto(ENROLL_SEARCH);  // done; back to the list (also clears capture)
    } else {
        beeper_error();
        ui_toast_show(r.message, false);
        ui_set_card_capture(on_enroll_card);  // let them try another card
    }
}

static void build_enroll_wait(void) {
    const char* name;
    const char* id;
    if (s_sel_is_new) {
        name = s_new_name;
        id = s_new_id;
    } else {
        const student_t* st = roster_student_at(s_sel_idx);
        name = st ? st->name : "?";
        id = st ? st->id : "?";
    }

    lv_obj_t* card = ui_make_card(s_content);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(card, 12, 0);
    lv_obj_set_style_pad_ver(card, 20, 0);

    ui_make_label(card, LV_SYMBOL_SD_CARD, THEME_ACCENT, &lv_font_montserrat_32);
    ui_make_label(card, "Tap the student's card", THEME_PRIMARY, &lv_font_montserrat_20);
    char who[80];
    snprintf(who, sizeof(who), "%s  (%s)", name, id);
    ui_make_label(card, who, THEME_TEXT, &lv_font_montserrat_14);
    ui_make_label(card, "Hold the RFID card near the reader", THEME_MUTED,
                  &lv_font_montserrat_14);

    lv_obj_t* cancel = ui_make_button(card, "Cancel", &theme_style_btn_outline,
                                      wait_cancel_cb, nullptr);
    lv_obj_set_width(cancel, LV_PCT(100));

    ui_set_card_capture(on_enroll_card);
}

static void goto_session_cb(lv_event_t*) {
    s_tab = TAB_SESSION;
    rebuild_content();
}

static void build_enroll(void) {
    // Enrollment checks the student into the current session, so it needs one
    // open. Show a prompt (with a shortcut) instead of the search otherwise.
    if (!session_open_here()) {
        lv_obj_t* card = ui_make_card(s_content);
        lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(card, 8, 0);
        ui_make_label(card, "No open session", THEME_PRIMARY, &lv_font_montserrat_20);
        lv_obj_t* m = ui_make_label(
            card,
            "Open a session in the Session tab first. Enrolled students are checked "
            "into it automatically.",
            THEME_MUTED, &lv_font_montserrat_14);
        lv_label_set_long_mode(m, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(m, LV_PCT(100));
        lv_obj_t* go = ui_make_button(card, "Go to Session", &theme_style_btn_primary,
                                      goto_session_cb, nullptr);
        lv_obj_set_width(go, LV_PCT(100));
        return;
    }

    switch (s_enroll_state) {
        case ENROLL_SEARCH:
            build_enroll_search();
            break;
        case ENROLL_MANUAL:
            build_enroll_manual();
            break;
        case ENROLL_WAIT:
            build_enroll_wait();
            break;
    }
}

static void enroll_goto(enroll_state_t st) {
    s_enroll_state = st;
    ui_set_card_capture(nullptr);
    // Release the shared keyboard BEFORE deleting the sub-state's widgets: the
    // enroll fields are textareas the keyboard points at, and cleaning them
    // while it still holds that pointer leaves a dangling ->ta that crashes the
    // next keyboard_hide()/keypress (use-after-free).
    keyboard_hide();
    lv_obj_clean(s_content);
    build_enroll();
}

// --- Enrollment lock / unlock -----------------------------------------------

static void update_lock_state(void) {
    // Header lock button: unlock glyph on green when unlocked, lock glyph on a
    // subtle background when locked.
    lv_obj_t* icon = lv_obj_get_child(s_lock_btn, 0);
    lv_label_set_text(icon, s_unlocked ? GLYPH_UNLOCK : GLYPH_LOCK);
    lv_obj_set_style_bg_color(s_lock_btn,
                              lv_color_hex(s_unlocked ? THEME_SUCCESS : THEME_ON_PRIMARY), 0);
    lv_obj_set_style_bg_opa(s_lock_btn, s_unlocked ? LV_OPA_COVER : LV_OPA_20, 0);
    // Dim the Enroll tab while locked.
    lv_obj_set_style_opa(s_tab_btns[TAB_ENROLL], s_unlocked ? LV_OPA_COVER : LV_OPA_50, 0);
}

static void close_unlock_modal(void) {
    if (s_unlock_modal) {
        keyboard_hide();
        ui_set_card_capture(nullptr);
        lv_obj_delete(s_unlock_modal);
        s_unlock_modal = nullptr;
        s_unlock_ta = nullptr;
    }
}

static void do_unlock(void) {
    s_unlocked = true;
    beeper_beep();
    close_unlock_modal();
    update_lock_state();
    if (s_unlock_goto_enroll) s_tab = TAB_ENROLL;
    rebuild_content();
}

// A professor card tap in the unlock modal.
static void on_unlock_card(const char* uid_hex) {
    teacher_t who;
    if (auth_lookup_uid(uid_hex, &who)) {
        do_unlock();
    } else {
        beeper_error();
        ui_toast_show("Not a professor card", false);
        ui_set_card_capture(on_unlock_card);  // let them try another card
    }
}

static void unlock_pw_ok_cb(lv_event_t*) {
    const char* pw = s_unlock_ta ? lv_textarea_get_text(s_unlock_ta) : "";
    teacher_t who;
    if (auth_lookup_password(pw, &who)) {
        do_unlock();
    } else {
        beeper_error();
        ui_toast_show("Wrong password", false);
    }
}

static void unlock_cancel_cb(lv_event_t*) {
    close_unlock_modal();
    rebuild_content();  // restore the current tab's card capture
}

static void open_unlock_modal(void) {
    if (s_unlock_modal) return;

    s_unlock_modal = lv_obj_create(s_sh.root);
    lv_obj_remove_style_all(s_unlock_modal);
    // Escape the shell's flex flow so this is a true full-screen overlay.
    lv_obj_add_flag(s_unlock_modal, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_set_size(s_unlock_modal, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_unlock_modal, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_unlock_modal, LV_OPA_50, 0);
    lv_obj_add_flag(s_unlock_modal, LV_OBJ_FLAG_CLICKABLE);  // swallow taps behind
    lv_obj_remove_flag(s_unlock_modal, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* card = ui_make_card(s_unlock_modal);
    lv_obj_set_width(card, LV_PCT(88));
    lv_obj_align(card, LV_ALIGN_TOP_MID, 0, 60);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 12, 0);

    ui_make_label(card, "Unlock enrollment", THEME_PRIMARY, &lv_font_montserrat_20);
    lv_obj_t* hint = ui_make_label(card, "Tap a professor card, or enter a password.",
                                   THEME_MUTED, &lv_font_montserrat_14);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(hint, LV_PCT(100));

    // Passwords are digits-only; use the numeric pad, pop it up right away, and
    // make its OK/finish button unlock (matching the idle login).
    s_unlock_ta = keyboard_make_textarea(card, "Password", 32, LV_KEYBOARD_MODE_NUMBER);
    lv_textarea_set_password_mode(s_unlock_ta, true);
    keyboard_show(s_unlock_ta, LV_KEYBOARD_MODE_NUMBER);
    keyboard_set_ready_cb(unlock_pw_ok_cb);

    lv_obj_t* row = lv_obj_create(card);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 10, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t* cancel = ui_make_button(row, "Cancel", &theme_style_btn_outline,
                                      unlock_cancel_cb, nullptr);
    lv_obj_set_flex_grow(cancel, 1);
    lv_obj_t* ok = ui_make_button(row, "Unlock", &theme_style_btn_primary, unlock_pw_ok_cb,
                                  nullptr);
    lv_obj_set_flex_grow(ok, 1);

    ui_set_card_capture(on_unlock_card);
}

static void lock_cb(lv_event_t*) {
    if (s_unlocked) {
        s_unlocked = false;  // re-lock
        update_lock_state();
        if (s_tab == TAB_ENROLL) {
            s_tab = TAB_SESSION;
            rebuild_content();
        }
    } else {
        s_unlock_goto_enroll = false;  // unlocked from the header, stay put
        open_unlock_modal();
    }
}

// Dims the kiosk button until a session is open.
static void update_kiosk_btn(void) {
    lv_obj_set_style_opa(s_kiosk_btn, session_open_here() ? LV_OPA_COVER : LV_OPA_40, 0);
}

// Kiosk mode (unattended student check-in) — only once a session is open.
static void kiosk_cb(lv_event_t*) {
    if (!session_open_here()) {
        ui_toast_show("Open a session to use kiosk mode", false);
        return;
    }
    scr_mgr_show(SCREEN_KIOSK, (void*)const_cast<class_rec_t*>(s_cls));
}

// --- Tabs / screen glue ------------------------------------------------------

static void rebuild_content(void) {
    // Leaving any enroll sub-state: no card should be captured for enroll.
    ui_set_card_capture(nullptr);
    keyboard_hide();

    // Swap only the active/idle look, preserving the button's height, flex
    // grow and press feedback (which lv_obj_remove_style_all would wipe).
    for (int i = 0; i < TAB_COUNT; i++) {
        lv_obj_remove_style(s_tab_btns[i], &theme_style_tab_active, 0);
        lv_obj_remove_style(s_tab_btns[i], &theme_style_tab_idle, 0);
        lv_obj_add_style(s_tab_btns[i],
                         i == s_tab ? &theme_style_tab_active : &theme_style_tab_idle, 0);
    }
    update_kiosk_btn();  // availability follows the session state

    lv_obj_clean(s_content);
    if (!s_cls) return;
    switch (s_tab) {
        case TAB_SESSION:
            build_session();
            break;
        case TAB_HISTORY:
            build_history();
            break;
        case TAB_ENROLL:
            s_enroll_state = ENROLL_SEARCH;  // always start at the search
            build_enroll();
            break;
        default:
            break;
    }
}

static void tab_cb(lv_event_t* e) {
    tab_id_t t = (tab_id_t)(uintptr_t)lv_event_get_user_data(e);
    if (t == TAB_ENROLL && !s_unlocked) {
        // Enrollment is locked; tapping it prompts to unlock first.
        s_unlock_goto_enroll = true;
        open_unlock_modal();
        return;
    }
    s_tab = t;
    rebuild_content();
}

static lv_obj_t* create(void) {
    // No footer nav here: a detail screen gets a back button instead.
    s_sh = shell_create("", "", false);
    shell_set_back(&s_sh, back_cb);

    // Header actions (right side): kiosk mode, then the enrollment lock.
    s_kiosk_btn = shell_add_action(&s_sh, GLYPH_KIOSK, &font_montserrat_custom_20, kiosk_cb,
                                   "Kiosk");
    s_lock_btn = shell_add_action(&s_sh, GLYPH_LOCK, &font_montserrat_custom_20, lock_cb,
                                  "Enroll");

    lv_obj_t* tabs = lv_obj_create(s_sh.body);
    lv_obj_remove_style_all(tabs);
    lv_obj_set_size(tabs, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(tabs, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(tabs, 8, 0);
    for (int i = 0; i < TAB_COUNT; i++) {
        s_tab_btns[i] = ui_make_button(tabs, TAB_NAMES[i], &theme_style_tab_idle, tab_cb,
                                       (void*)(uintptr_t)i);
        lv_obj_set_flex_grow(s_tab_btns[i], 1);
    }

    s_content = lv_obj_create(s_sh.body);
    lv_obj_remove_style_all(s_content);
    lv_obj_set_size(s_content, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_content, 10, 0);

    return s_sh.root;
}

static void on_show(void* arg) {
    if (arg) {
        s_cls = (const class_rec_t*)arg;
        s_tab = TAB_SESSION;
        s_unlocked = false;  // re-lock enrollment on each class entry
    }
    update_lock_state();
    update_kiosk_btn();
    lv_label_set_text(s_sh.title, s_cls ? s_cls->name : "?");
    char subtitle[88];
    if (s_cls) {
        snprintf(subtitle, sizeof(subtitle), "%s  |  %s", s_cls->code, s_cls->schedule);
    } else {
        snprintf(subtitle, sizeof(subtitle), "Student attendance");
    }
    lv_label_set_text(s_sh.subtitle, subtitle);
    rebuild_content();
}

static void on_hide(void) {
    keyboard_hide();
    ui_set_card_capture(nullptr);
}

const screen_t scr_class = {
    .create = create,
    .on_show = on_show,
    .on_hide = on_hide,
};
