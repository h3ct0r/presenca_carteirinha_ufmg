#include "ui/screens/scr_student.h"

#include "ui/screen_manager.h"
#include "ui/theme/theme.h"

static constexpr uint32_t COL_BG = THEME_DARK_BG;
static constexpr uint32_t COL_TEXT = THEME_DARK_TEXT;
static constexpr uint32_t COL_MUTED = THEME_DARK_MUTED;
static constexpr uint32_t COL_OK = THEME_DARK_OK;

// How long the confirmation stays up before falling back to idle.
static constexpr uint32_t SHOW_MS = 4000;

static lv_obj_t* s_uid_label = nullptr;
static lv_timer_t* s_back_timer = nullptr;

static void back_to_idle_cb(lv_timer_t*) {
    // scr_mgr_show triggers our on_hide, which deletes the timer; nothing
    // may touch it after this call.
    scr_mgr_show(SCREEN_IDLE, nullptr);
}

static lv_obj_t* create(void) {
    lv_obj_t* root = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(root, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_text_color(root, lv_color_hex(COL_TEXT), 0);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(root, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(root, 24, 0);

    // Green check badge.
    lv_obj_t* badge = lv_obj_create(root);
    lv_obj_set_size(badge, 110, 110);
    lv_obj_set_style_radius(badge, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(badge, lv_color_hex(COL_OK), 0);
    lv_obj_set_style_border_width(badge, 0, 0);
    lv_obj_remove_flag(badge, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* check = lv_label_create(badge);
    lv_label_set_text(check, LV_SYMBOL_OK);
    lv_obj_set_style_text_font(check, &lv_font_montserrat_32, 0);
    lv_obj_center(check);

    lv_obj_t* title = lv_label_create(root);
    lv_label_set_text(title, "Presence recorded");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_32, 0);

    // Placeholder identity line until the SD student lookup exists.
    s_uid_label = lv_label_create(root);
    lv_obj_set_style_text_font(s_uid_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_uid_label, lv_color_hex(COL_MUTED), 0);

    return root;
}

static void on_show(void* arg) {
    const scr_student_arg_t* a = (const scr_student_arg_t*)arg;
    lv_label_set_text(s_uid_label, a ? a->uid_hex : "?");

    // Periodic (not one-shot) on purpose: a one-shot timer frees itself
    // after its callback, which would collide with on_hide freeing it too.
    // In practice it fires once — the callback navigates away.
    s_back_timer = lv_timer_create(back_to_idle_cb, SHOW_MS, nullptr);
}

static void on_hide(void) {
    if (s_back_timer) {
        lv_timer_delete(s_back_timer);
        s_back_timer = nullptr;
    }
}

const screen_t scr_student = {
    .create = create,
    .on_show = on_show,
    .on_hide = on_hide,
};
