#include "ui/screens/scr_class_stats.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app/class_stats.h"
#include "app/roster.h"
#include "services/roster_service.h"
#include "storage/attendance_store.h"
#include "ui/components/keyboard.h"
#include "ui/components/shell.h"
#include "ui/components/student_photo.h"
#include "ui/components/toast.h"
#include "ui/screen_manager.h"
#include "ui/sd_resync.h"
#include "ui/theme/theme.h"

static shell_t s_sh;
static lv_obj_t* s_content = nullptr;  // rebuilt on_show
static const class_rec_t* s_cls = nullptr;

// Settings widgets (valid only while the settings card is built).
static lv_obj_t* s_name_ta = nullptr;
static lv_obj_t* s_sched_ta = nullptr;
static lv_obj_t* s_capture_sw = nullptr;
static lv_obj_t* s_fv_ta = nullptr;
static lv_obj_t* s_timed_sw = nullptr;
static lv_obj_t* s_min_ta = nullptr;
static uint32_t s_sel_color = 0;

// Preset class colors (the swatch row). The active one gets a light border.
static const uint32_t PALETTE[] = {0x272766, 0xD64545, 0x1F9D55, 0xE8890C,
                                   0x3B6FE0, 0x8E44AD, 0x16A085, 0x555555};
static constexpr int PALETTE_N = (int)(sizeof(PALETTE) / sizeof(PALETTE[0]));

// Past sessions averaged for the attendance rate. Matches the class screen's cap.
static constexpr int MAX_DATES = 24;

static lv_obj_t* s_swatches[PALETTE_N] = {};

static void back_cb(lv_event_t*) { scr_mgr_show(SCREEN_CLASSES, nullptr); }

// The shared keyboard floats over the body, so reserve its height as bottom
// padding — but only while it is up, or the settings list ends in dead space.
// Same approach as the class screen's apply_keyboard_pad().
static void apply_keyboard_pad(void) {
    if (!s_sh.body) return;
    lv_obj_set_style_pad_bottom(s_sh.body, keyboard_is_visible() ? KEYBOARD_PAD : UI_PAD, 0);
}

static void kb_visibility_cb(bool) { apply_keyboard_pad(); }

// --- statistics -------------------------------------------------------------

static void build_stats(lv_obj_t* parent) {
    lv_obj_t* card = ui_make_card(parent);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 6, 0);
    ui_make_label(card, "Statistics", THEME_PRIMARY, &lv_font_montserrat_20);

    int total = s_cls->roster_count;
    int with_photo = student_photo_count_for_class(s_cls);

    char line[96];
    snprintf(line, sizeof(line), "%d students   %d with photo", total, with_photo);
    ui_make_label(card, line, THEME_TEXT, &lv_font_montserrat_14);

    // Attendance rate: mean of each recorded session's present percentage.
    // attendance_present_for() memoises, so the repeat visit costs no SD reads.
    char dates[MAX_DATES][12];
    int nd = attendance_list_dates(s_cls->dir, dates, MAX_DATES);
    if (nd > 0 && total > 0) {
        long pct_sum = 0;
        for (int i = 0; i < nd; i++) {
            int present = attendance_present_for(s_cls->dir, dates[i]);
            pct_sum += (present * 100 + total / 2) / total;
        }
        snprintf(line, sizeof(line), "Avg attendance %ld%% over %d session(s)", pct_sum / nd, nd);
    } else {
        snprintf(line, sizeof(line), "No sessions recorded yet");
    }
    ui_make_label(card, line, THEME_MUTED, &lv_font_montserrat_14);

    char turma[160];
    if (class_turma_breakdown(s_cls, turma, sizeof(turma))) {
        lv_obj_t* tl = ui_make_label(card, turma, THEME_MUTED, &lv_font_montserrat_14);
        lv_label_set_long_mode(tl, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(tl, LV_PCT(100));
    }
}

// --- settings ---------------------------------------------------------------

static void highlight_swatches(void) {
    for (int i = 0; i < PALETTE_N; i++) {
        bool on = (PALETTE[i] == s_sel_color);
        lv_obj_set_style_border_width(s_swatches[i], on ? 3 : 0, 0);
        lv_obj_set_style_border_color(s_swatches[i], lv_color_hex(THEME_TEXT), 0);
    }
}

static void swatch_cb(lv_event_t* e) {
    s_sel_color = (uint32_t)(uintptr_t)lv_event_get_user_data(e);
    highlight_swatches();
}

static void save_cb(lv_event_t*) {
    keyboard_hide();
    // The six widgets are built and destroyed together, so they are either all
    // live or all gone; check once rather than guarding some and not others.
    if (!s_name_ta || !s_sched_ta || !s_capture_sw || !s_fv_ta || !s_timed_sw || !s_min_ta) {
        return;
    }
    const char* name = lv_textarea_get_text(s_name_ta);
    const char* sched = lv_textarea_get_text(s_sched_ta);
    bool capture = lv_obj_has_state(s_capture_sw, LV_STATE_CHECKED);
    int fv = atoi(lv_textarea_get_text(s_fv_ta));
    if (fv <= 0) fv = FACE_VERIFY_SECONDS_DEFAULT;
    bool timed = lv_obj_has_state(s_timed_sw, LV_STATE_CHECKED);
    int min_min = atoi(lv_textarea_get_text(s_min_ta));
    if (min_min < 1) min_min = MIN_ATTENDANCE_MIN_DEFAULT;
    roster_result_t r = roster_class_update_settings(s_cls->code, name, sched, s_sel_color, capture,
                                                     fv, timed, min_min);
    ui_toast_show(r.message, r.ok);
    if (r.ok) scr_mgr_show(SCREEN_CLASSES, nullptr);  // list reflects the new name/color
}

static void build_settings(lv_obj_t* parent) {
    lv_obj_t* card = ui_make_card(parent);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 8, 0);
    ui_make_label(card, "Settings", THEME_PRIMARY, &lv_font_montserrat_20);

    ui_make_label(card, "Class name", THEME_TEXT, &lv_font_montserrat_14);
    s_name_ta = keyboard_make_textarea(card, "Class name", 47, LV_KEYBOARD_MODE_TEXT_UPPER);
    lv_textarea_set_text(s_name_ta, s_cls->name);

    ui_make_label(card, "Schedule", THEME_TEXT, &lv_font_montserrat_14);
    s_sched_ta = keyboard_make_textarea(card, "e.g. Tue/Thu 10-12", 39, LV_KEYBOARD_MODE_TEXT_UPPER);
    lv_textarea_set_text(s_sched_ta, s_cls->schedule);

    ui_make_label(card, "Color", THEME_TEXT, &lv_font_montserrat_14);
    lv_obj_t* row = lv_obj_create(card);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 8, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    s_sel_color = s_cls->color;
    for (int i = 0; i < PALETTE_N; i++) {
        lv_obj_t* sw = lv_obj_create(row);
        lv_obj_remove_flag(sw, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(sw, 32, 32);
        lv_obj_set_style_radius(sw, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(sw, lv_color_hex(PALETTE[i]), 0);
        lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, 0);
        lv_obj_add_flag(sw, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(sw, swatch_cb, LV_EVENT_CLICKED, (void*)(uintptr_t)PALETTE[i]);
        s_swatches[i] = sw;
    }
    highlight_swatches();

    // Photo check-in (kiosk face verify): a switch + its countdown seconds.
    lv_obj_t* crow = lv_obj_create(card);
    lv_obj_remove_style_all(crow);
    lv_obj_set_size(crow, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(crow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(crow, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(crow, LV_OBJ_FLAG_SCROLLABLE);
    ui_make_label(crow, "Photo check-in (kiosk) Warn: beta-tested only", THEME_TEXT, &lv_font_montserrat_14);
    s_capture_sw = lv_switch_create(crow);
    if (s_cls->capture_photos) lv_obj_add_state(s_capture_sw, LV_STATE_CHECKED);

    ui_make_label(card, "Face-verify time (seconds)", THEME_TEXT, &lv_font_montserrat_14);
    s_fv_ta = keyboard_make_textarea(card, "15", 3, LV_KEYBOARD_MODE_NUMBER);
    char fvbuf[8];
    snprintf(fvbuf, sizeof(fvbuf), "%d", s_cls->face_verify_seconds);
    lv_textarea_set_text(s_fv_ta, fvbuf);

    // Timed attendance: a switch + the minimum-minutes threshold.
    lv_obj_t* trow = lv_obj_create(card);
    lv_obj_remove_style_all(trow);
    lv_obj_set_size(trow, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(trow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(trow, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(trow, LV_OBJ_FLAG_SCROLLABLE);
    ui_make_label(trow, "Timed attendance (tap to arrive, tap to confirm)", THEME_TEXT,
                  &lv_font_montserrat_14);
    s_timed_sw = lv_switch_create(trow);
    if (s_cls->timed_attendance) lv_obj_add_state(s_timed_sw, LV_STATE_CHECKED);

    ui_make_label(card, "Minimum minutes present", THEME_TEXT, &lv_font_montserrat_14);
    s_min_ta = keyboard_make_textarea(card, "45", 4, LV_KEYBOARD_MODE_NUMBER);
    char minbuf[8];
    snprintf(minbuf, sizeof(minbuf), "%d", s_cls->min_attendance_min);
    lv_textarea_set_text(s_min_ta, minbuf);

    lv_obj_t* save = ui_make_button(card, "Save settings", &theme_style_btn_primary, save_cb,
                                    nullptr);
    lv_obj_set_width(save, LV_PCT(100));
}

// --- screen -----------------------------------------------------------------

static lv_obj_t* create(void) {
    s_sh = shell_create("", "Statistics & settings", false);
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
    keyboard_set_visibility_cb(kb_visibility_cb);
    keyboard_hide();  // release before cleaning the keyboard textareas (UAF guard)
    lv_obj_clean(s_content);
    if (!s_cls) return;
    lv_label_set_text(s_sh.title, s_cls->name);
    apply_keyboard_pad();
    build_stats(s_content);
    build_settings(s_content);
}

static void on_hide(void) {
    keyboard_set_visibility_cb(nullptr);  // never fire against a torn-down layout
    keyboard_hide();
}

const screen_t scr_class_stats = {
    .create = create,
    .on_show = on_show,
    .on_hide = on_hide,
};
