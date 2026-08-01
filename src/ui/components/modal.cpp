#include "ui/components/modal.h"

#include "ui/theme/theme.h"

// Card width, as a percentage of the screen. One number so every modal on the
// device is the same width — the copies had drifted to 86 and 88.
static constexpr int CARD_PCT = 88;

lv_obj_t* ui_modal_create(lv_obj_t* parent, int top_y, lv_obj_t** out_card) {
    lv_obj_t* scrim = lv_obj_create(parent);
    lv_obj_remove_style_all(scrim);
    // Not laid out by the parent: see modal.h. Every shell root is a flex
    // container, so without this the scrim becomes a full-height flex child and
    // squashes the screen behind it.
    lv_obj_add_flag(scrim, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_set_size(scrim, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(scrim, lv_color_hex(THEME_SCRIM), 0);
    lv_obj_set_style_bg_opa(scrim, LV_OPA_50, 0);
    lv_obj_add_flag(scrim, LV_OBJ_FLAG_CLICKABLE);  // swallow taps behind
    lv_obj_remove_flag(scrim, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* card = ui_make_card(scrim);
    lv_obj_set_width(card, LV_PCT(CARD_PCT));
    if (top_y == UI_MODAL_CENTER) {
        lv_obj_align(card, LV_ALIGN_CENTER, 0, 0);
    } else {
        // Pinned near the top so the shared keyboard, which floats over the
        // bottom half, cannot cover the fields.
        lv_obj_align(card, LV_ALIGN_TOP_MID, 0, top_y);
    }
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 12, 0);

    if (out_card) *out_card = card;
    return scrim;
}

lv_obj_t* ui_modal_title(lv_obj_t* card, const char* text, uint32_t color) {
    return ui_make_label(card, text, color, &lv_font_montserrat_20);
}

lv_obj_t* ui_modal_body(lv_obj_t* card, const char* text) {
    lv_obj_t* l = ui_make_label(card, text, THEME_MUTED, &lv_font_montserrat_14);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(l, LV_PCT(100));
    return l;
}

void ui_modal_actions(lv_obj_t* card, const char* ok_text, lv_style_t* ok_style,
                      lv_event_cb_t on_ok, lv_event_cb_t on_cancel) {
    lv_obj_t* row = lv_obj_create(card);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 10, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* cancel = ui_make_button(row, "Cancel", &theme_style_btn_outline, on_cancel,
                                      nullptr);
    lv_obj_set_flex_grow(cancel, 1);
    lv_obj_t* ok = ui_make_button(row, ok_text, ok_style, on_ok, nullptr);
    lv_obj_set_flex_grow(ok, 1);
}
