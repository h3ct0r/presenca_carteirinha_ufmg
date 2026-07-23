#include "ui/components/toast.h"

#include <stdio.h>

#include "ui/components/status_bar.h"
#include "ui/theme/theme.h"

static constexpr uint32_t DISMISS_MS = 5000;

static lv_obj_t* s_toast = nullptr;
static lv_timer_t* s_timer = nullptr;

static void dismiss(void) {
    if (s_timer) {
        lv_timer_delete(s_timer);
        s_timer = nullptr;
    }
    if (s_toast) {
        lv_obj_delete(s_toast);
        s_toast = nullptr;
    }
}

static void timer_cb(lv_timer_t*) { dismiss(); }
static void click_cb(lv_event_t*) { dismiss(); }

void ui_toast_show(const char* msg, bool ok) {
    dismiss();  // replace any current toast

    s_toast = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_toast);
    lv_obj_set_width(s_toast, LV_PCT(92));
    lv_obj_set_height(s_toast, LV_SIZE_CONTENT);
    lv_obj_align(s_toast, LV_ALIGN_TOP_MID, 0, STATUS_BAR_HEIGHT + 8);
    lv_obj_set_style_bg_color(s_toast, lv_color_hex(ok ? THEME_SUCCESS : THEME_DANGER), 0);
    lv_obj_set_style_bg_opa(s_toast, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_toast, 10, 0);
    lv_obj_set_style_pad_all(s_toast, 14, 0);
    lv_obj_set_style_shadow_width(s_toast, 16, 0);
    lv_obj_set_style_shadow_opa(s_toast, LV_OPA_30, 0);
    lv_obj_remove_flag(s_toast, LV_OBJ_FLAG_SCROLLABLE);
    // Tap anywhere on it to dismiss immediately.
    lv_obj_add_flag(s_toast, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_toast, click_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t* lbl = lv_label_create(s_toast);
    char full[160];
    snprintf(full, sizeof(full), "%s  %s", ok ? LV_SYMBOL_OK : LV_SYMBOL_WARNING, msg);
    lv_label_set_text(lbl, full);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0xFFFFFF), 0);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(lbl, LV_PCT(100));

    s_timer = lv_timer_create(timer_cb, DISMISS_MS, nullptr);
}
