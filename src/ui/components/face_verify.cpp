#include "ui/components/face_verify.h"

#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "lvgl.h"
#include "services/face_detection_service.h"
#include "storage/checkin_store.h"
#include "storage/photo_store.h"
#include "ui/theme/theme.h"

static lv_obj_t* s_overlay = nullptr;
static lv_timer_t* s_timer = nullptr;
static lv_obj_t* s_img = nullptr;
static lv_obj_t* s_boxes[FACE_MAX_BOXES];
static lv_obj_t* s_count_lbl = nullptr;
static lv_obj_t* s_status_lbl = nullptr;

static lv_image_dsc_t s_dsc;
static uint8_t* s_buf = nullptr;  // preview RGB565, reused across verifies

static char s_id[20];
static char s_date[12];
static char s_code[24];
static face_verify_done_cb s_done = nullptr;
static int64_t s_start_us = 0;
static int64_t s_timeout_us = 0;

// Tears down the overlay + timer (keeps the preview buffer for reuse). Returns
// the pending callback so the caller can fire it after the overlay is gone.
static face_verify_done_cb take_down(void) {
    if (s_timer) {
        lv_timer_t* t = s_timer;
        s_timer = nullptr;
        lv_timer_delete(t);
    }
    if (s_overlay) {
        lv_obj_delete(s_overlay);
        s_overlay = nullptr;
    }
    face_verify_done_cb cb = s_done;
    s_done = nullptr;
    return cb;
}

static void finish(bool verified) {
    face_verify_done_cb cb = take_down();
    if (cb) cb(verified);
}

static void verify_tick(lv_timer_t*) {
    face_box_t boxes[FACE_MAX_BOXES];
    int n = face_detection_snapshot(s_buf, boxes, FACE_MAX_BOXES);
    if (n >= 0) lv_obj_invalidate(s_img);  // same buffer, new pixels

    for (int i = 0; i < FACE_MAX_BOXES; i++) {
        if (n >= 0 && i < n) {
            lv_obj_set_pos(s_boxes[i], boxes[i].x, boxes[i].y);
            lv_obj_set_size(s_boxes[i], boxes[i].w, boxes[i].h);
            lv_obj_remove_flag(s_boxes[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_boxes[i], LV_OBJ_FLAG_HIDDEN);
        }
    }

    int64_t now = esp_timer_get_time();
    int remain = (int)((s_timeout_us - (now - s_start_us) + 999999) / 1000000);
    if (remain < 0) remain = 0;

    if (n >= 1) {
        // Face seen: grab this frame as the check-in photo, then finish success.
        lv_label_set_text(s_status_lbl, LV_SYMBOL_OK "  Face detected");
        lv_obj_set_style_text_color(s_status_lbl, lv_color_hex(THEME_SUCCESS), 0);
        char path[96];
        if (checkin_store_next_path(s_id, s_date, s_code, path, sizeof(path))) {
            photo_store_encode_to(path, s_buf, FACE_PREVIEW_W, FACE_PREVIEW_H);
        }
        finish(true);
        return;
    }

    char c[8];
    snprintf(c, sizeof(c), "%d", remain);
    lv_label_set_text(s_count_lbl, c);
    if (now - s_start_us >= s_timeout_us) {
        finish(false);
    }
}

void face_verify_open(const char* student_id, const char* student_name, const char* date,
                      const char* class_code, int timeout_s, face_verify_done_cb done) {
    if (s_overlay) return;  // one at a time
    if (!s_buf) {
        s_buf = (uint8_t*)heap_caps_aligned_calloc(128, 1, FACE_PREVIEW_W * FACE_PREVIEW_H * 2,
                                                   MALLOC_CAP_SPIRAM);
    }
    if (!s_buf) {  // no preview buffer -> can't verify; fail open to the caller
        if (done) done(false);
        return;
    }

    snprintf(s_id, sizeof(s_id), "%s", student_id ? student_id : "");
    snprintf(s_date, sizeof(s_date), "%s", date ? date : "");
    snprintf(s_code, sizeof(s_code), "%s", class_code ? class_code : "");
    s_done = done;
    s_start_us = esp_timer_get_time();
    s_timeout_us = (int64_t)(timeout_s > 0 ? timeout_s : 1) * 1000000;

    s_overlay = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_overlay);
    lv_obj_set_size(s_overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_overlay, lv_color_hex(THEME_DARK_BG), 0);
    lv_obj_set_style_bg_opa(s_overlay, LV_OPA_COVER, 0);
    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(s_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(s_overlay, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_overlay, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_top(s_overlay, 40, 0);
    lv_obj_set_style_pad_row(s_overlay, 14, 0);

    char who[64];
    snprintf(who, sizeof(who), "%s", student_name ? student_name : "");
    lv_obj_t* title = lv_label_create(s_overlay);
    lv_label_set_text(title, "Show your face to the camera");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(THEME_DARK_TEXT), 0);
    lv_obj_t* namel = lv_label_create(s_overlay);
    lv_label_set_text(namel, who);
    lv_obj_set_style_text_font(namel, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(namel, lv_color_hex(THEME_DARK_MUTED), 0);

    // Live preview at native size so the face-box coordinates map 1:1.
    lv_obj_t* holder = lv_obj_create(s_overlay);
    lv_obj_remove_style_all(holder);
    lv_obj_set_size(holder, FACE_PREVIEW_W, FACE_PREVIEW_H);
    lv_obj_remove_flag(holder, LV_OBJ_FLAG_SCROLLABLE);

    s_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    s_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    s_dsc.header.w = FACE_PREVIEW_W;
    s_dsc.header.h = FACE_PREVIEW_H;
    s_dsc.header.stride = FACE_PREVIEW_W * 2;
    s_dsc.data_size = FACE_PREVIEW_W * FACE_PREVIEW_H * 2;
    s_dsc.data = s_buf;
    s_img = lv_image_create(holder);
    lv_obj_set_pos(s_img, 0, 0);
    lv_image_set_src(s_img, &s_dsc);

    for (int i = 0; i < FACE_MAX_BOXES; i++) {
        lv_obj_t* b = lv_obj_create(holder);
        lv_obj_set_style_bg_opa(b, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_color(b, lv_color_hex(THEME_SUCCESS), 0);
        lv_obj_set_style_border_width(b, 3, 0);
        lv_obj_set_style_radius(b, 4, 0);
        lv_obj_remove_flag(b, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
        lv_obj_add_flag(b, LV_OBJ_FLAG_HIDDEN);
        s_boxes[i] = b;
    }

    s_count_lbl = lv_label_create(s_overlay);
    lv_label_set_text(s_count_lbl, "");
    lv_obj_set_style_text_font(s_count_lbl, &lv_font_montserrat_32, 0);
    lv_obj_set_style_text_color(s_count_lbl, lv_color_hex(THEME_DARK_ACCENT), 0);

    s_status_lbl = lv_label_create(s_overlay);
    lv_label_set_text(s_status_lbl, "Looking for a face...");
    lv_obj_set_style_text_font(s_status_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_status_lbl, lv_color_hex(THEME_DARK_MUTED), 0);

    s_timer = lv_timer_create(verify_tick, 100, nullptr);
}

void face_verify_cancel(void) { take_down(); }
