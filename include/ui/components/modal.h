#pragma once

#include <lvgl.h>

// The dimmed full-screen overlay with a card in it, used by every confirm,
// password prompt and "tap a card" flow. It was copy-pasted across ~12 sites in
// 6 files; each copy repeated the same thirteen lines and each was a chance to
// forget one of them — which is how the IGNORE_LAYOUT misrender happened.
//
// The caller still OWNS the returned scrim: it keeps its own pointer, and closes
// by deleting it. Deliberately not a singleton — several screens hold more than
// one modal pointer, and their open/close guards (`if (s_x) return;`) depend on
// each being tracked separately.
//
// LVGL thread only.

// Card offset from the top of the screen, in pixels. Pass UI_MODAL_CENTER to
// centre it vertically instead (for short, informational modals).
constexpr int UI_MODAL_CENTER = -1;

// Creates the scrim on `parent` and a card inside it. Returns the SCRIM — store
// it and `lv_obj_delete()` it to close. `*out_card` receives the card, which is
// a flex column ready for content.
//
// The scrim always carries LV_OBJ_FLAG_IGNORE_LAYOUT: a parent that lays out its
// children (every shell root, and the idle screen's flex column) would otherwise
// treat this full-height object as another flex child and squash the content
// behind it. It is also CLICKABLE, so a tap cannot reach whatever is underneath.
lv_obj_t* ui_modal_create(lv_obj_t* parent, int top_y, lv_obj_t** out_card);

// Heading line. `color` is a THEME_* constant — THEME_PRIMARY for a normal
// prompt, THEME_DANGER for a destructive one.
lv_obj_t* ui_modal_title(lv_obj_t* card, const char* text, uint32_t color);

// Wrapped explanatory paragraph under the title.
lv_obj_t* ui_modal_body(lv_obj_t* card, const char* text);

// The bottom row: "Cancel" on the left, `ok_text` on the right, both half width.
// `ok_style` is usually &theme_style_btn_primary, or &theme_style_btn_danger for
// something destructive.
void ui_modal_actions(lv_obj_t* card, const char* ok_text, lv_style_t* ok_style,
                      lv_event_cb_t on_ok, lv_event_cb_t on_cancel);
