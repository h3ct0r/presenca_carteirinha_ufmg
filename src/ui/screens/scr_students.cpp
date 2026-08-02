#include "ui/screens/scr_students.h"

#include <stdio.h>
#include <string.h>

#include "app/roster.h"
#include "services/roster_service.h"
#include "storage/attendance_store.h"
#include "ui/components/keyboard.h"
#include "ui/components/modal.h"
#include "ui/components/shell.h"
#include "ui/components/toast.h"
#include "ui/screen_manager.h"
#include "ui/screens/scr_class.h"
#include "ui/sd_resync.h"
#include "ui/theme/theme.h"

// Two states, same body: the registry list and the new-student form. Same shape
// as the class screen's enroll sub-flow, and torn down with the same discipline.
typedef enum { STATE_LIST, STATE_FORM } state_t;

// The registry holds up to ROSTER_MAX_STUDENTS (600) and the LVGL pool is fixed,
// so this list caps what it DRAWS and tells the user to keep typing — the same
// bargain the enroll search makes (scr_class.cpp ENROLL_MAX_ROWS). Exhausting
// the pool does not throw: LV_USE_ASSERT_MALLOC halts the UI thread.
static constexpr int MAX_ROWS = 12;

static shell_t s_sh;
static lv_obj_t* s_content = nullptr;
static const class_rec_t* s_cls = nullptr;
static state_t s_state = STATE_LIST;

// Widgets, valid only while their state is built.
static lv_obj_t* s_search_ta = nullptr;
static lv_obj_t* s_results = nullptr;
static lv_obj_t* s_form_id_ta = nullptr;
static lv_obj_t* s_form_name_ta = nullptr;
static lv_obj_t* s_form_turma_ta = nullptr;

static lv_obj_t* s_confirm = nullptr;  // the "add to class?" modal scrim
static int s_confirm_idx = -1;         // registry index it is asking about

static void goto_state(state_t st);
static void update_results(void);

// EVERY static pointing into s_content, dropped together before it is cleaned.
static void forget_widgets(void) {
    s_search_ta = nullptr;
    s_results = nullptr;
    s_form_id_ta = nullptr;
    s_form_name_ta = nullptr;
    s_form_turma_ta = nullptr;
}

static void close_confirm(void) {
    if (!s_confirm) return;
    lv_obj_delete(s_confirm);
    s_confirm = nullptr;
    s_confirm_idx = -1;
}

// The shared keyboard floats over the body, so reserve its height as bottom
// padding only while it is actually up.
static void apply_keyboard_pad(void) {
    if (!s_sh.body) return;
    lv_obj_set_style_pad_bottom(s_sh.body, keyboard_is_visible() ? KEYBOARD_PAD : UI_PAD, 0);
}

static void kb_visibility_cb(bool) { apply_keyboard_pad(); }

static bool session_open_here(void) {
    return s_cls && attendance_is_open() && strcmp(attendance_dir(), s_cls->dir) == 0;
}

static bool in_this_class(int student_idx) {
    if (!s_cls) return false;
    for (int i = 0; i < s_cls->roster_count; i++) {
        if (s_cls->roster[i] == student_idx) return true;
    }
    return false;
}

// Adding always happens during an open session (the registry is reached from the
// enroll view, which requires one), so check the student in right away — exactly
// what the card enroll path does.
static void report(const roster_result_t& r, const char* student_id) {
    char msg[128];
    const char* suffix = "";
    if (r.ok && student_id && student_id[0] && session_open_here()) {
        attendance_set(student_id, true);
        suffix = " - checked in";
    }
    snprintf(msg, sizeof(msg), "%s%s", r.message, suffix);
    ui_toast_show(msg, r.ok);
}

// --- confirm modal ----------------------------------------------------------

static void confirm_cancel_cb(lv_event_t*) { close_confirm(); }

static void confirm_ok_cb(lv_event_t*) {
    int idx = s_confirm_idx;
    close_confirm();
    const student_t* st = roster_student_at(idx);
    if (!st || !s_cls) return;
    char id[sizeof(st->id)];  // report() uses it after the write
    snprintf(id, sizeof(id), "%s", st->id);
    roster_result_t r = roster_class_add_existing(s_cls->code, idx, nullptr);
    report(r, id);
    // Only the rows change (the student's now "In this class"); rebuilding the
    // whole view would drop the search text with it.
    if (r.ok) update_results();
}

static void row_cb(lv_event_t* e) {
    if (s_confirm) return;  // one at a time
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    const student_t* st = roster_student_at(idx);
    if (!st || !s_cls) return;
    keyboard_hide();
    s_confirm_idx = idx;

    lv_obj_t* card = nullptr;
    s_confirm = ui_modal_create(s_sh.root, UI_MODAL_CENTER, &card);
    ui_modal_title(card, "Add to this class?", THEME_PRIMARY);
    char body[192];
    snprintf(body, sizeof(body), "%s (%s) will be added to %s%s.", st->name, st->id, s_cls->name,
             session_open_here() ? " and checked into this session" : "");
    ui_modal_body(card, body);
    ui_modal_actions(card, "Add", &theme_style_btn_primary, confirm_ok_cb, confirm_cancel_cb);
}

// --- list -------------------------------------------------------------------

static void update_results(void) {
    if (!s_results) return;
    lv_obj_clean(s_results);
    const char* q = s_search_ta ? lv_textarea_get_text(s_search_ta) : "";

    int matched = 0;  // every match
    int shown = 0;    // rows actually built
    for (int i = 0; i < roster_student_count(); i++) {
        const student_t* st = roster_student_at(i);
        if (!st) continue;
        if (!ui_text_contains(st->name, q) && !ui_text_contains(st->id, q)) continue;

        matched++;
        if (shown >= MAX_ROWS) continue;  // counted, not drawn

        bool here = in_this_class(i);
        lv_obj_t* row = ui_make_card(s_results);
        lv_obj_set_style_pad_all(row, 10, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(row, 10, 0);
        if (!here) {
            lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(row, row_cb, LV_EVENT_CLICKED, (void*)(intptr_t)i);
            ui_add_press_feedback(row);
        }

        lv_obj_t* col = lv_obj_create(row);
        lv_obj_remove_style_all(col);
        // Let taps fall through to the row's click handler — lv_obj_create()
        // hands out CLICKABLE and remove_style_all() does not take it back, so
        // without this the name and id area swallows the tap.
        lv_obj_remove_flag(col, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_height(col, LV_SIZE_CONTENT);
        // Takes the width the trailing status label leaves, so a long name wraps
        // inside the row instead of pushing it off the edge.
        lv_obj_set_flex_grow(col, 1);
        lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(col, 2, 0);
        ui_label_fit(ui_make_label(col, st->name, THEME_TEXT, &lv_font_montserrat_16));
        char sub[72];
        snprintf(sub, sizeof(sub), "%s%s", st->id, st->rfid_uid[0] ? "   card registered" : "");
        ui_label_fit(ui_make_label(col, sub, st->rfid_uid[0] ? THEME_SUCCESS : THEME_MUTED,
                                   &lv_font_montserrat_14));

        ui_make_label(row, here ? "In this class" : LV_SYMBOL_PLUS "  Add",
                      here ? THEME_SUCCESS : THEME_PRIMARY, &lv_font_montserrat_14);
        shown++;
    }

    if (shown > 0) {
        // State the true total, and say plainly when the list is cut short —
        // otherwise a missing student looks like they are not registered.
        char head[96];
        bool truncated = matched > shown;
        if (truncated) {
            snprintf(head, sizeof(head), "Showing %d of %d - keep typing to narrow the search",
                     shown, matched);
        } else {
            snprintf(head, sizeof(head), "%d student%s on this card", matched,
                     matched == 1 ? "" : "s");
        }
        lv_obj_t* count = ui_make_label(s_results, head, truncated ? THEME_WARNING : THEME_MUTED,
                                        &lv_font_montserrat_14);
        lv_label_set_long_mode(count, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(count, LV_PCT(100));
        lv_obj_move_to_index(count, 0);
        return;
    }

    lv_obj_t* card = ui_make_card(s_results);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 8, 0);
    lv_obj_t* msg = ui_make_label(card,
                                  q[0] ? "No student on this card matches. Add them as a new one."
                                       : "No students registered on this card yet.",
                                  THEME_MUTED, &lv_font_montserrat_14);
    lv_label_set_long_mode(msg, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(msg, LV_PCT(100));
}

static void search_changed_cb(lv_event_t*) { update_results(); }

static void new_student_cb(lv_event_t*) { goto_state(STATE_FORM); }

static void build_list(void) {
    // Pin the search box and the New student button, scroll only the results.
    lv_obj_set_height(s_content, LV_PCT(100));

    lv_obj_t* card = ui_make_card(s_content);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 10, 0);

    s_search_ta = keyboard_make_textarea(card, "Search every student by name or ID", 47,
                                         LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_obj_add_event_cb(s_search_ta, search_changed_cb, LV_EVENT_VALUE_CHANGED, nullptr);

    lv_obj_t* add = ui_make_button(card, LV_SYMBOL_PLUS "  New student",
                                   &theme_style_btn_outline, new_student_cb, nullptr);
    lv_obj_set_width(add, LV_PCT(100));

    s_results = lv_obj_create(s_content);
    lv_obj_remove_style_all(s_results);
    lv_obj_set_width(s_results, LV_PCT(100));
    lv_obj_set_flex_grow(s_results, 1);
    lv_obj_set_flex_flow(s_results, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_results, 8, 0);
    lv_obj_set_style_pad_right(s_results, 4, 0);  // keep rows clear of the scrollbar
    lv_obj_add_flag(s_results, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_results, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_results, LV_SCROLLBAR_MODE_AUTO);
    update_results();
}

// --- new-student form -------------------------------------------------------

static void form_cancel_cb(lv_event_t*) { goto_state(STATE_LIST); }

static void form_save_cb(lv_event_t*) {
    keyboard_hide();
    if (!s_form_id_ta || !s_form_name_ta || !s_form_turma_ta || !s_cls) return;
    char id[24];
    snprintf(id, sizeof(id), "%s", lv_textarea_get_text(s_form_id_ta));
    roster_result_t r = roster_class_add_new(s_cls->code, id, lv_textarea_get_text(s_form_name_ta),
                                             lv_textarea_get_text(s_form_turma_ta));
    report(r, id);
    // On failure the form stays up with the values in it, so a rejected id or
    // name can be corrected instead of retyped.
    if (r.ok) goto_state(STATE_LIST);
}

static void build_form(void) {
    lv_obj_t* card = ui_make_card(s_content);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 8, 0);

    ui_make_label(card, "New student", THEME_PRIMARY, &lv_font_montserrat_20);
    lv_obj_t* hint = ui_make_label(card,
                                   "No card needed - it binds itself the first time they tap.",
                                   THEME_MUTED, &lv_font_montserrat_14);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(hint, LV_PCT(100));

    ui_make_label(card, "Student ID", THEME_TEXT, &lv_font_montserrat_14);
    s_form_id_ta = keyboard_make_textarea(card, "e.g. 2024-0123", 19, LV_KEYBOARD_MODE_NUMBER);

    ui_make_label(card, "Full name", THEME_TEXT, &lv_font_montserrat_14);
    s_form_name_ta = keyboard_make_textarea(card, "Student name", 47, LV_KEYBOARD_MODE_TEXT_LOWER);

    ui_make_label(card, "Turma (optional)", THEME_TEXT, &lv_font_montserrat_14);
    s_form_turma_ta = keyboard_make_textarea(card, "e.g. TE1", 15, LV_KEYBOARD_MODE_TEXT_UPPER);

    lv_obj_t* save = ui_make_button(card, "Add to class", &theme_style_btn_primary, form_save_cb,
                                    nullptr);
    lv_obj_set_width(save, LV_PCT(100));
    lv_obj_t* cancel = ui_make_button(card, "Cancel", &theme_style_btn_outline, form_cancel_cb,
                                      nullptr);
    lv_obj_set_width(cancel, LV_PCT(100));
}

// --- screen -----------------------------------------------------------------

static void update_header(void) {
    lv_label_set_text(s_sh.title, s_state == STATE_FORM ? "New student" : "Student registry");
    char sub[96];
    snprintf(sub, sizeof(sub), "%s | %s", s_cls ? s_cls->name : "",
             s_state == STATE_FORM ? "not on the card yet" : "add a student to this class");
    lv_label_set_text(s_sh.subtitle, sub);
}

static void rebuild(void) {
    // Release the shared keyboard BEFORE deleting the textareas it points at.
    keyboard_hide();
    close_confirm();
    forget_widgets();
    lv_obj_clean(s_content);
    lv_obj_set_height(s_content, LV_SIZE_CONTENT);  // the list overrides this
    update_header();
    apply_keyboard_pad();
    if (!s_cls) return;
    if (s_state == STATE_FORM) {
        build_form();
    } else {
        build_list();
    }
}

static void goto_state(state_t st) {
    s_state = st;
    rebuild();
}

static void back_cb(lv_event_t*) {
    if (s_state == STATE_FORM) {
        goto_state(STATE_LIST);
        return;
    }
    // Back into the enroll view this was opened from, not the class hub.
    scr_class_request_enroll_view();
    scr_mgr_show(SCREEN_CLASS, (void*)const_cast<class_rec_t*>(s_cls));
}

static lv_obj_t* create(void) {
    s_sh = shell_create("Student registry", "", false);
    shell_set_back(&s_sh, back_cb);

    s_content = lv_obj_create(s_sh.body);
    lv_obj_remove_style_all(s_content);
    lv_obj_set_size(s_content, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_content, 10, 0);
    // Let scroll gestures bubble up to sh.body instead of being caught here.
    lv_obj_remove_flag(s_content, LV_OBJ_FLAG_SCROLLABLE);
    return s_sh.root;
}

static void on_show(void* arg) {
    if (arg) s_cls = (const class_rec_t*)arg;
    // Attendance only: s_cls points into roster storage, which a reload rewrites.
    ui_sd_resync_light();
    s_state = STATE_LIST;
    keyboard_set_visibility_cb(kb_visibility_cb);
    rebuild();
}

static void on_hide(void) {
    keyboard_set_visibility_cb(nullptr);  // never fire against a torn-down layout
    keyboard_hide();
    close_confirm();
}

const screen_t scr_students = {
    .create = create,
    .on_show = on_show,
    .on_hide = on_hide,
};
