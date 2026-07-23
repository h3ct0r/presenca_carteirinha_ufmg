#include "ui/theme/theme.h"

#include "audio/beeper.h"

lv_style_t theme_style_card;
lv_style_t theme_style_btn_primary;
lv_style_t theme_style_btn_outline;
lv_style_t theme_style_btn_accent;
lv_style_t theme_style_btn_danger;
lv_style_t theme_style_input;
lv_style_t theme_style_tab_active;
lv_style_t theme_style_tab_idle;
lv_style_t theme_style_pressed;

// Press feedback dims the object; the transition eases it in/out so it reads
// as a tap. Opacity only (no transform): transforms shrink the hit area in
// LVGL 9's transform-aware input, which can make buttons miss taps.
static const lv_style_prop_t s_press_props[] = {LV_STYLE_OPA, LV_STYLE_PROP_INV};
static lv_style_transition_dsc_t s_press_trans;

static void init_button_base(lv_style_t* s, uint32_t bg, uint32_t text) {
    lv_style_init(s);
    lv_style_set_radius(s, 12);
    lv_style_set_bg_color(s, lv_color_hex(bg));
    lv_style_set_bg_opa(s, LV_OPA_COVER);
    lv_style_set_border_width(s, 0);
    lv_style_set_text_color(s, lv_color_hex(text));
    lv_style_set_pad_ver(s, 10);
    lv_style_set_pad_hor(s, 14);
    lv_style_set_transition(s, &s_press_trans);
}

void theme_init(void) {
    // Must come before init_button_base(), which references s_press_trans.
    lv_style_transition_dsc_init(&s_press_trans, s_press_props, lv_anim_path_ease_out, 120, 0,
                                 nullptr);

    // The single "pressed" look reused across the whole app: dim the object.
    // Opacity is color-agnostic, so one style gives consistent feedback on
    // every button style and clickable object.
    lv_style_init(&theme_style_pressed);
    lv_style_set_opa(&theme_style_pressed, LV_OPA_60);
    lv_style_set_transition(&theme_style_pressed, &s_press_trans);

    lv_style_init(&theme_style_card);
    lv_style_set_radius(&theme_style_card, 14);
    lv_style_set_bg_color(&theme_style_card, lv_color_hex(THEME_SURFACE));
    lv_style_set_bg_opa(&theme_style_card, LV_OPA_COVER);
    lv_style_set_border_color(&theme_style_card, lv_color_hex(THEME_BORDER));
    lv_style_set_border_width(&theme_style_card, 1);
    lv_style_set_shadow_width(&theme_style_card, 16);
    lv_style_set_shadow_opa(&theme_style_card, LV_OPA_20);
    lv_style_set_shadow_color(&theme_style_card, lv_color_hex(0x000000));
    lv_style_set_pad_all(&theme_style_card, 12);
    lv_style_set_text_color(&theme_style_card, lv_color_hex(THEME_TEXT));
    lv_style_set_transition(&theme_style_card, &s_press_trans);  // clickable cards ease back

    init_button_base(&theme_style_btn_primary, THEME_PRIMARY, THEME_ON_PRIMARY);
    init_button_base(&theme_style_btn_accent, THEME_ACCENT, THEME_ON_PRIMARY);
    init_button_base(&theme_style_btn_danger, THEME_DANGER, THEME_ON_PRIMARY);

    init_button_base(&theme_style_btn_outline, THEME_SURFACE, THEME_PRIMARY);
    lv_style_set_border_color(&theme_style_btn_outline, lv_color_hex(THEME_PRIMARY));
    lv_style_set_border_width(&theme_style_btn_outline, 2);

    lv_style_init(&theme_style_input);
    lv_style_set_radius(&theme_style_input, 10);
    lv_style_set_bg_color(&theme_style_input, lv_color_hex(THEME_SOFT));
    lv_style_set_bg_opa(&theme_style_input, LV_OPA_COVER);
    lv_style_set_border_color(&theme_style_input, lv_color_hex(THEME_BORDER));
    lv_style_set_border_width(&theme_style_input, 1);
    lv_style_set_pad_all(&theme_style_input, 10);
    lv_style_set_text_color(&theme_style_input, lv_color_hex(THEME_TEXT));

    lv_style_init(&theme_style_tab_active);
    lv_style_set_radius(&theme_style_tab_active, 10);
    lv_style_set_bg_color(&theme_style_tab_active, lv_color_hex(THEME_PRIMARY));
    lv_style_set_bg_opa(&theme_style_tab_active, LV_OPA_COVER);
    lv_style_set_text_color(&theme_style_tab_active, lv_color_hex(THEME_ON_PRIMARY));
    lv_style_set_border_width(&theme_style_tab_active, 0);
    lv_style_set_pad_ver(&theme_style_tab_active, 8);
    lv_style_set_pad_hor(&theme_style_tab_active, 6);  // keep label off the rounded edges
    lv_style_set_text_font(&theme_style_tab_active, &lv_font_montserrat_20);
    lv_style_set_transition(&theme_style_tab_active, &s_press_trans);

    lv_style_init(&theme_style_tab_idle);
    lv_style_set_radius(&theme_style_tab_idle, 10);
    lv_style_set_bg_color(&theme_style_tab_idle, lv_color_hex(THEME_SURFACE));
    lv_style_set_bg_opa(&theme_style_tab_idle, LV_OPA_COVER);
    lv_style_set_text_color(&theme_style_tab_idle, lv_color_hex(THEME_MUTED));
    lv_style_set_border_color(&theme_style_tab_idle, lv_color_hex(THEME_BORDER));
    lv_style_set_border_width(&theme_style_tab_idle, 1);
    lv_style_set_pad_ver(&theme_style_tab_idle, 8);
    lv_style_set_pad_hor(&theme_style_tab_idle, 6);
    lv_style_set_text_font(&theme_style_tab_idle, &lv_font_montserrat_20);
    lv_style_set_transition(&theme_style_tab_idle, &s_press_trans);
}

static void beep_on_click_cb(lv_event_t*) { beeper_touch(); }

void ui_add_press_feedback(lv_obj_t* obj) {
    lv_obj_add_style(obj, &theme_style_pressed, LV_STATE_PRESSED);
    // Audible tap feedback. Fires on a completed click (not on scroll), and
    // only for objects that opt in here, so the on-screen keyboard's keys —
    // which don't use this helper — stay silent.
    lv_obj_add_event_cb(obj, beep_on_click_cb, LV_EVENT_CLICKED, nullptr);
}

lv_obj_t* ui_make_label(lv_obj_t* parent, const char* text, uint32_t color,
                        const lv_font_t* font) {
    lv_obj_t* label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    if (font) lv_obj_set_style_text_font(label, font, 0);
    return label;
}

lv_obj_t* ui_make_button(lv_obj_t* parent, const char* text, lv_style_t* style,
                         lv_event_cb_t cb, void* user_data) {
    lv_obj_t* btn = lv_button_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_add_style(btn, style, 0);
    lv_obj_set_height(btn, UI_BUTTON_HEIGHT);  // standard height, app-wide
    ui_add_press_feedback(btn);
    if (cb) lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);

    lv_obj_t* label = lv_label_create(btn);
    lv_label_set_text(label, text);
    lv_obj_center(label);
    return btn;
}

lv_obj_t* ui_make_card(lv_obj_t* parent) {
    lv_obj_t* card = lv_obj_create(parent);
    lv_obj_remove_style_all(card);
    lv_obj_add_style(card, &theme_style_card, 0);
    lv_obj_set_width(card, LV_PCT(100));
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    return card;
}
