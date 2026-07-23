#include "ui/screens/scr_teacher_reg.h"

#include "ui/components/keyboard.h"
#include "ui/components/status_bar.h"
#include "ui/screen_manager.h"
#include "ui/theme/theme.h"

static lv_obj_t* s_ta_id = nullptr;
static lv_obj_t* s_ta_name = nullptr;
static lv_obj_t* s_ta_email = nullptr;
static lv_obj_t* s_status = nullptr;

static void confirm_cb(lv_event_t*) {
    keyboard_hide();
    if (lv_textarea_get_text(s_ta_id)[0] == '\0') {
        lv_label_set_text(s_status, "Enter the teacher ID.");
        lv_obj_set_style_text_color(s_status, lv_color_hex(THEME_DANGER), 0);
        return;
    }
    if (lv_textarea_get_text(s_ta_name)[0] == '\0') {
        lv_label_set_text(s_status, "Enter the full name.");
        lv_obj_set_style_text_color(s_status, lv_color_hex(THEME_DANGER), 0);
        return;
    }
    scr_mgr_show(SCREEN_ADMIN, nullptr);  // demo: nothing persisted
}

static void cancel_cb(lv_event_t*) { scr_mgr_show(SCREEN_ADMIN, nullptr); }

static lv_obj_t* create(void) {
    lv_obj_t* root = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(root, lv_color_hex(THEME_PRIMARY_DARK), 0);
    lv_obj_set_style_pad_top(root, STATUS_BAR_HEIGHT + 8, 0);
    lv_obj_set_style_pad_hor(root, 12, 0);
    lv_obj_set_style_pad_row(root, 12, 0);
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(root, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scroll_dir(root, LV_DIR_VER);

    ui_make_label(root, "Teacher Registration", THEME_ON_PRIMARY, &lv_font_montserrat_20);

    lv_obj_t* card = ui_make_card(root);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 8, 0);

    ui_make_label(card, "Teacher ID", THEME_TEXT, &lv_font_montserrat_14);
    s_ta_id = keyboard_make_textarea(card, "e.g. 745012", 15, LV_KEYBOARD_MODE_NUMBER);

    ui_make_label(card, "Full name", THEME_TEXT, &lv_font_montserrat_14);
    s_ta_name = keyboard_make_textarea(card, "Your name", 47, LV_KEYBOARD_MODE_TEXT_LOWER);

    ui_make_label(card, "Email (optional)", THEME_TEXT, &lv_font_montserrat_14);
    s_ta_email = keyboard_make_textarea(card, "name@university.edu", 47,
                                        LV_KEYBOARD_MODE_TEXT_LOWER);

    ui_make_label(card, "RFID card", THEME_TEXT, &lv_font_montserrat_14);
    lv_obj_t* wait_box = lv_obj_create(card);
    lv_obj_remove_style_all(wait_box);
    lv_obj_add_style(wait_box, &theme_style_input, 0);
    lv_obj_set_size(wait_box, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_remove_flag(wait_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_center(
        ui_make_label(wait_box, "Waiting for card...", THEME_MUTED, &lv_font_montserrat_14));

    lv_obj_t* confirm = ui_make_button(card, "Confirm Registration",
                                       &theme_style_btn_primary, confirm_cb, nullptr);
    lv_obj_set_width(confirm, LV_PCT(100));
    lv_obj_t* cancel =
        ui_make_button(card, "Cancel", &theme_style_btn_outline, cancel_cb, nullptr);
    lv_obj_set_width(cancel, LV_PCT(100));

    s_status = ui_make_label(card, "", THEME_MUTED, &lv_font_montserrat_14);
    return root;
}

static void on_show(void*) {
    lv_textarea_set_text(s_ta_id, "");
    lv_textarea_set_text(s_ta_name, "");
    lv_textarea_set_text(s_ta_email, "");
    lv_label_set_text(s_status, "");
}

static void on_hide(void) { keyboard_hide(); }

const screen_t scr_teacher_reg = {
    .create = create,
    .on_show = on_show,
    .on_hide = on_hide,
};
