#include "ui/components/progress.h"

#include <stdio.h>
#include <string.h>

#include "ui/theme/theme.h"

// The caller is blocking the LVGL thread, so every visible update costs a full
// synchronous redraw. An import writing ~1200 files would spend longer
// repainting than importing, so coalesce: at most one redraw per this interval.
static constexpr uint32_t REPAINT_MS = 100;

static lv_obj_t* s_overlay = nullptr;
static lv_obj_t* s_context = nullptr;
static lv_obj_t* s_stage = nullptr;
static lv_obj_t* s_detail = nullptr;
static lv_obj_t* s_bar = nullptr;
static uint32_t s_last_paint = 0;
// Last STAGE drawn — not the detail. A new stage always repaints, so a short one
// is never skipped entirely (which would leave the operator watching a stale
// label and conclude it had stalled there). The detail changes every single
// iteration, so including it here would force a redraw per file and defeat the
// throttle completely.
static char s_last_stage[48] = "";

void ui_progress_open(const char* title) {
    if (s_overlay) {  // reuse: re-opening must not stack overlays
        lv_label_set_text(lv_obj_get_child(s_overlay, 0), title ? title : "");
        return;
    }

    s_overlay = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_overlay);
    lv_obj_set_size(s_overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_overlay, lv_color_hex(THEME_DARK_BG), 0);
    lv_obj_set_style_bg_opa(s_overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(s_overlay, 28, 0);
    lv_obj_set_flex_flow(s_overlay, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_overlay, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(s_overlay, 12, 0);
    // Swallows taps so a stray press can't reach the screen underneath. It also
    // makes leaving this object behind catastrophic — see ui_progress_close().
    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(s_overlay, LV_OBJ_FLAG_SCROLLABLE);

    // Child 0 — ui_progress_open() retargets this one on reuse.
    lv_obj_t* head = lv_label_create(s_overlay);
    lv_label_set_text(head, title ? title : "");
    lv_obj_set_style_text_color(head, lv_color_hex(THEME_DARK_TEXT), 0);
    lv_obj_set_style_text_font(head, &lv_font_montserrat_20, 0);
    lv_label_set_long_mode(head, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(head, LV_PCT(100));
    lv_obj_set_style_text_align(head, LV_TEXT_ALIGN_CENTER, 0);

    s_context = lv_label_create(s_overlay);
    lv_label_set_text(s_context, "");
    lv_obj_set_style_text_color(s_context, lv_color_hex(THEME_DARK_ACCENT), 0);
    lv_obj_set_style_text_font(s_context, &lv_font_montserrat_14, 0);
    lv_obj_add_flag(s_context, LV_OBJ_FLAG_HIDDEN);

    s_bar = lv_bar_create(s_overlay);
    lv_obj_set_size(s_bar, LV_PCT(100), 10);
    lv_bar_set_range(s_bar, 0, 100);
    lv_bar_set_value(s_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_bar, lv_color_hex(THEME_DARK_MUTED), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_bar, LV_OPA_30, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_bar, lv_color_hex(THEME_DARK_ACCENT), LV_PART_INDICATOR);
    lv_obj_add_flag(s_bar, LV_OBJ_FLAG_HIDDEN);

    s_stage = lv_label_create(s_overlay);
    lv_label_set_text(s_stage, "");
    lv_obj_set_style_text_color(s_stage, lv_color_hex(THEME_DARK_TEXT), 0);
    lv_obj_set_style_text_font(s_stage, &lv_font_montserrat_14, 0);
    lv_label_set_long_mode(s_stage, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_stage, LV_PCT(100));
    lv_obj_set_style_text_align(s_stage, LV_TEXT_ALIGN_CENTER, 0);

    // The item being worked on. Dimmer and clipped to one line: it scrolls past
    // far too quickly to read, and is there to prove movement, not to be read.
    s_detail = lv_label_create(s_overlay);
    lv_label_set_text(s_detail, "");
    lv_obj_set_style_text_color(s_detail, lv_color_hex(THEME_DARK_MUTED), 0);
    lv_obj_set_style_text_font(s_detail, &lv_font_montserrat_14, 0);
    lv_label_set_long_mode(s_detail, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_detail, LV_PCT(100));
    lv_obj_set_style_text_align(s_detail, LV_TEXT_ALIGN_CENTER, 0);

    s_last_stage[0] = '\0';
    s_last_paint = 0;  // 0 = "never painted", so the first set() always draws
    lv_refr_now(NULL);
}

void ui_progress_context(const char* line) {
    if (!s_context) return;
    if (line && line[0]) {
        lv_label_set_text(s_context, line);
        lv_obj_remove_flag(s_context, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_context, LV_OBJ_FLAG_HIDDEN);
    }
}

void ui_progress_set(const char* stage, const char* detail, int done, int total) {
    if (!s_overlay) return;
    const char* st = stage ? stage : "";

    // A new stage always draws; within a stage, coalesce on time. lv_tick_elaps
    // handles the 32-bit tick wrap; s_last_paint == 0 means nothing drawn yet.
    const bool new_stage = strncmp(st, s_last_stage, sizeof(s_last_stage) - 1) != 0;
    if (!new_stage && s_last_paint && lv_tick_elaps(s_last_paint) < REPAINT_MS) return;

    if (new_stage) snprintf(s_last_stage, sizeof(s_last_stage), "%s", st);

    char line[80];
    if (total > 0) {
        snprintf(line, sizeof(line), "%s  %d/%d", st, done, total);
    } else if (done > 0) {
        snprintf(line, sizeof(line), "%s  %d", st, done);  // running count, no total
    } else {
        snprintf(line, sizeof(line), "%s", st);
    }
    lv_label_set_text(s_stage, line);
    lv_label_set_text(s_detail, detail ? detail : "");

    if (total > 0) {
        int pct = (int)(((long)done * 100 + total / 2) / total);
        if (pct < 0) pct = 0;
        if (pct > 100) pct = 100;
        lv_bar_set_value(s_bar, pct, LV_ANIM_OFF);
        lv_obj_remove_flag(s_bar, LV_OBJ_FLAG_HIDDEN);
    } else {
        // Nothing meaningful to fill: hide it rather than show an empty bar,
        // which reads as "0% and stuck".
        lv_obj_add_flag(s_bar, LV_OBJ_FLAG_HIDDEN);
    }

    s_last_paint = lv_tick_get();
    lv_refr_now(NULL);  // the caller never returns to lv_timer_handler()
}

void ui_progress_close(void) {
    if (!s_overlay) return;
    lv_obj_delete(s_overlay);  // takes the children with it
    s_overlay = nullptr;
    s_context = nullptr;
    s_stage = nullptr;
    s_detail = nullptr;
    s_bar = nullptr;
    s_last_stage[0] = '\0';
    s_last_paint = 0;
}

void ui_progress_cb(const progress_t* p, void*) {
    if (!p) return;
    ui_progress_set(p->stage, p->detail, p->done, p->total);
}
