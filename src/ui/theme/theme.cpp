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

// Press feedback darkens and dims the object. No transform: transforms shrink
// the hit area in LVGL 9's transform-aware input, which can make buttons miss
// taps.
static const lv_style_prop_t s_press_props[] = {LV_STYLE_OPA, LV_STYLE_RECOLOR_OPA,
                                                LV_STYLE_PROP_INV};

// Two descriptors, because the two directions want opposite timing: the press
// must land under the finger, the release should ease so it doesn't blink.
//
// LVGL makes this separable. On a state change it collects transitions ONLY
// from styles that apply in the NEW state, and for a property the higher state
// selector wins (lv_obj.c, lv_obj_update_state). Entering PRESSED, the pressed
// style outranks the base style, so IN governs; returning to default, the
// pressed style is skipped entirely, so OUT governs.
static lv_style_transition_dsc_t s_press_in_trans;   // on theme_style_pressed
static lv_style_transition_dsc_t s_press_out_trans;  // on every base style

static void init_button_base(lv_style_t* s, uint32_t bg, uint32_t text) {
    lv_style_init(s);
    lv_style_set_radius(s, 12);
    lv_style_set_bg_color(s, lv_color_hex(bg));
    lv_style_set_bg_opa(s, LV_OPA_COVER);
    lv_style_set_border_width(s, 0);
    lv_style_set_text_color(s, lv_color_hex(text));
    lv_style_set_pad_ver(s, 10);
    lv_style_set_pad_hor(s, 14);
    lv_style_set_transition(s, &s_press_out_trans);
}

void theme_init(void) {
    // Must come before init_button_base(), which references s_press_out_trans.
    lv_style_transition_dsc_init(&s_press_in_trans, s_press_props, lv_anim_path_linear, 0, 0,
                                 nullptr);
    lv_style_transition_dsc_init(&s_press_out_trans, s_press_props, lv_anim_path_ease_out, 120, 0,
                                 nullptr);

    // The single "pressed" look reused across the whole app. Both properties are
    // color-agnostic, so one style gives consistent feedback on every button
    // style and clickable object: recolor darkens whatever the widget draws
    // (which a plain opacity dim cannot do on a saturated button — it only makes
    // it paler), and the opacity keeps the softening the UI already had.
    lv_style_init(&theme_style_pressed);
    lv_style_set_recolor(&theme_style_pressed, lv_color_black());
    lv_style_set_recolor_opa(&theme_style_pressed, LV_OPA_20);
    lv_style_set_opa(&theme_style_pressed, LV_OPA_80);
    lv_style_set_transition(&theme_style_pressed, &s_press_in_trans);

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
    lv_style_set_transition(&theme_style_card, &s_press_out_trans);  // clickable cards ease back

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
    lv_style_set_transition(&theme_style_tab_active, &s_press_out_trans);

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
    lv_style_set_transition(&theme_style_tab_idle, &s_press_out_trans);
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

void ui_label_fit(lv_obj_t* label) {
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);

    // The layout check has to come first: lv_obj_get_style_flex_flow() reports
    // LV_FLEX_FLOW_ROW (0x00) for a parent that isn't flex at all, which would
    // otherwise make every label in a plain container a grow item.
    lv_obj_t* parent = lv_obj_get_parent(label);
    bool row = false;
    if (parent && lv_obj_get_style_layout(parent, LV_PART_MAIN) == LV_LAYOUT_FLEX) {
        switch (lv_obj_get_style_flex_flow(parent, LV_PART_MAIN)) {
            case LV_FLEX_FLOW_ROW:
            case LV_FLEX_FLOW_ROW_WRAP:
            case LV_FLEX_FLOW_ROW_REVERSE:
            case LV_FLEX_FLOW_ROW_WRAP_REVERSE:
                row = true;
                break;
            default:
                break;
        }
    }

    // A grow item's width is decided by the layout, so LVGL stops measuring the
    // text to size it — exactly the bound wrapping needs.
    if (row) {
        lv_obj_set_flex_grow(label, 1);
    } else {
        lv_obj_set_width(label, LV_PCT(100));
    }
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
