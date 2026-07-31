#include "ui/screens/scr_classes.h"

#include <stdio.h>

#include "app/session.h"
#include "services/roster_service.h"
#include "ui/components/shell.h"
#include "ui/screen_manager.h"
#include "ui/screens/scr_class.h"
#include "ui/screens/scr_class_stats.h"
#include "ui/sd_resync.h"
#include "ui/theme/theme.h"

static lv_obj_t* s_subtitle = nullptr;  // shows the logged-in professor
static lv_obj_t* s_list = nullptr;      // rebuilt on_show from the SD roster

static void open_class_cb(lv_event_t* e) {
    // Roster classes live in the service's static storage, so handing the
    // pointer through on_show is safe.
    scr_mgr_show(SCREEN_CLASS, lv_event_get_user_data(e));
}

static void open_stats_cb(lv_event_t* e) {
    scr_mgr_show(SCREEN_CLASS_STATS, lv_event_get_user_data(e));
}

static void add_class_card(lv_obj_t* body, const class_rec_t* cls) {
    lv_obj_t* card = ui_make_card(body);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(card, open_class_cb, LV_EVENT_CLICKED,
                        (void*)const_cast<class_rec_t*>(cls));
    ui_add_press_feedback(card);

    // Colored band across the top, as in the legacy design.
    lv_obj_t* band = lv_obj_create(card);
    lv_obj_remove_style_all(band);
    lv_obj_set_size(band, LV_PCT(100), 6);
    lv_obj_align(band, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(band, lv_color_hex(cls->color), 0);
    lv_obj_set_style_bg_opa(band, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(band, 3, 0);

    lv_obj_t* row = lv_obj_create(card);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(row, 12, 0);
    lv_obj_set_style_pad_top(row, 16, 0);
    lv_obj_set_style_pad_column(row, 12, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    // Let taps fall through to the card's click handler.
    lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* icon = lv_obj_create(row);
    lv_obj_remove_style_all(icon);
    lv_obj_set_size(icon, 40, 40);
    lv_obj_set_style_bg_color(icon, lv_color_hex(cls->color), 0);
    lv_obj_set_style_bg_opa(icon, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(icon, 10, 0);
    char initial[2] = {cls->name[0], '\0'};
    lv_obj_center(ui_make_label(icon, initial, THEME_ON_PRIMARY, &lv_font_montserrat_20));

    lv_obj_t* col = lv_obj_create(row);
    lv_obj_remove_style_all(col);
    lv_obj_set_size(col, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(col, 1);  // take the width, pushing the gear to the edge
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(col, 2, 0);
    lv_obj_remove_flag(col, LV_OBJ_FLAG_CLICKABLE);

    ui_make_label(col, cls->name, THEME_TEXT, &lv_font_montserrat_14);
    char detail[112];
    snprintf(detail, sizeof(detail), "%s  |  %s  |  %d students", cls->code, cls->schedule,
             cls->roster_count);
    ui_make_label(col, detail, THEME_MUTED, &lv_font_montserrat_14);

    // Gear button → statistics & per-class settings. It's clickable, so a tap on
    // it targets the gear (not the card's open handler underneath).
    lv_obj_t* gear = lv_button_create(row);
    lv_obj_remove_style_all(gear);
    lv_obj_set_size(gear, 56, 56);
    lv_obj_set_style_radius(gear, 12, 0);
    lv_obj_set_style_bg_color(gear, lv_color_hex(THEME_BORDER), 0);
    lv_obj_set_style_bg_opa(gear, LV_OPA_40, 0);
    lv_obj_add_event_cb(gear, open_stats_cb, LV_EVENT_CLICKED,
                        (void*)const_cast<class_rec_t*>(cls));
    ui_add_press_feedback(gear);
    lv_obj_center(ui_make_label(gear, LV_SYMBOL_SETTINGS, THEME_TEXT, &lv_font_montserrat_32));
}

static void add_notice_card(const char* title, const char* msg) {
    lv_obj_t* card = ui_make_card(s_list);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 6, 0);
    ui_make_label(card, title, THEME_TEXT, &lv_font_montserrat_14);
    lv_obj_t* m = ui_make_label(card, msg, THEME_MUTED, &lv_font_montserrat_14);
    lv_label_set_long_mode(m, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(m, LV_PCT(100));
}

static void build_list(void) {
    lv_obj_clean(s_list);

    if (roster_get_status() != ROSTER_OK) {
        char err[160];
        roster_get_error(err, sizeof(err));
        add_notice_card("No class data", err[0] ? err : "Check the SD card data files.");
        return;
    }

    const teacher_t* t = session_get();
    const char* email = t ? t->email : "";

    // A class folder that couldn't be parsed is skipped rather than blanking the
    // whole list (e.g. a leftover folder from a previous config). Say so, or the
    // class would just be quietly missing.
    int skipped = roster_skipped_class_count();
    if (skipped > 0) {
        char why[160], msg[240];
        roster_get_skip_reason(why, sizeof(why));
        snprintf(msg, sizeof(msg), "%d class folder(s) on the SD card could not be read: %s",
                 skipped, why[0] ? why : "see the serial log");
        add_notice_card("Some classes were skipped", msg);
    }

    int shown = 0;
    for (int i = 0; i < roster_class_count(); i++) {
        const class_rec_t* cls = roster_class_at(i);
        if (!cls || !roster_class_matches_teacher(cls, email)) continue;
        add_class_card(s_list, cls);
        shown++;
    }

    if (shown == 0) {
        char msg[120];
        snprintf(msg, sizeof(msg),
                 "No classes are assigned to %s on the SD card.",
                 email[0] ? email : "this account");
        add_notice_card("No classes", msg);
    }
}

static lv_obj_t* create(void) {
    shell_t sh = shell_create("My Classes", "", true);
    shell_set_active_nav(&sh, SCREEN_CLASSES);
    s_subtitle = sh.subtitle;

    s_list = lv_obj_create(sh.body);
    lv_obj_remove_style_all(s_list);
    lv_obj_set_size(s_list, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_list, 10, 0);

    return sh.root;
}

static void on_show(void*) {
    // Safe place for the roster reload: the list below is rebuilt from scratch
    // and this screen holds no class_rec_t* across the call.
    ui_sd_resync_full();
    const teacher_t* t = session_get();
    char subtitle[80];
    snprintf(subtitle, sizeof(subtitle), "Prof. %s", t ? t->name : "");
    lv_label_set_text(s_subtitle, subtitle);
    build_list();
}

const screen_t scr_classes = {
    .create = create,
    .on_show = on_show,
    .on_hide = nullptr,
};
