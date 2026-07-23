#include "ui/screens/scr_class_form.h"

#include "ui/components/keyboard.h"
#include "ui/components/shell.h"
#include "ui/screen_manager.h"
#include "ui/theme/theme.h"

static lv_obj_t* s_ta_code = nullptr;
static lv_obj_t* s_ta_name = nullptr;
static lv_obj_t* s_ta_schedule = nullptr;
static lv_obj_t* s_status = nullptr;

static void submit_cb(lv_event_t*) {
    keyboard_hide();
    if (lv_textarea_get_text(s_ta_code)[0] == '\0') {
        lv_label_set_text(s_status, "Enter the class code.");
        lv_obj_set_style_text_color(s_status, lv_color_hex(THEME_DANGER), 0);
        return;
    }
    if (lv_textarea_get_text(s_ta_name)[0] == '\0') {
        lv_label_set_text(s_status, "Enter the class name.");
        lv_obj_set_style_text_color(s_status, lv_color_hex(THEME_DANGER), 0);
        return;
    }
    scr_mgr_show(SCREEN_CLASSES, nullptr);  // demo: nothing persisted
}

static void cancel_cb(lv_event_t*) { scr_mgr_show(SCREEN_CLASSES, nullptr); }

static lv_obj_t* create(void) {
    shell_t sh = shell_create("New Class", "Create a class", false);

    lv_obj_t* card = ui_make_card(sh.body);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 8, 0);

    ui_make_label(card, "Class code", THEME_TEXT, &lv_font_montserrat_14);
    s_ta_code = keyboard_make_textarea(card, "e.g. CS101-M1", 15, LV_KEYBOARD_MODE_TEXT_LOWER);

    ui_make_label(card, "Name", THEME_TEXT, &lv_font_montserrat_14);
    s_ta_name = keyboard_make_textarea(card, "e.g. Data Structures", 47,
                                       LV_KEYBOARD_MODE_TEXT_LOWER);

    ui_make_label(card, "Schedule", THEME_TEXT, &lv_font_montserrat_14);
    s_ta_schedule = keyboard_make_textarea(card, "e.g. Tue/Thu 10:00-12:00", 31,
                                           LV_KEYBOARD_MODE_TEXT_LOWER);

    lv_obj_t* submit =
        ui_make_button(card, "Create Class", &theme_style_btn_primary, submit_cb, nullptr);
    lv_obj_set_width(submit, LV_PCT(100));
    lv_obj_t* cancel =
        ui_make_button(card, "Cancel", &theme_style_btn_outline, cancel_cb, nullptr);
    lv_obj_set_width(cancel, LV_PCT(100));

    s_status = ui_make_label(card, "", THEME_MUTED, &lv_font_montserrat_14);
    return sh.root;
}

static void on_show(void*) {
    lv_textarea_set_text(s_ta_code, "");
    lv_textarea_set_text(s_ta_name, "");
    lv_textarea_set_text(s_ta_schedule, "");
    lv_label_set_text(s_status, "");
}

static void on_hide(void) { keyboard_hide(); }

const screen_t scr_class_form = {
    .create = create,
    .on_show = on_show,
    .on_hide = on_hide,
};
