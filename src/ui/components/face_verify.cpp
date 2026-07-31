#include "ui/components/face_verify.h"

#include <stdio.h>
#include <string.h>

#include "esp32-hal-log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "lvgl.h"
#include "services/face_detection_service.h"
#include "storage/checkin_store.h"
#include "storage/photo_store.h"
#include "ui/components/student_photo.h"
#include "ui/theme/theme.h"

static const char* TAG = "face_verify";

static lv_obj_t* s_overlay = nullptr;
static lv_timer_t* s_timer = nullptr;
static lv_obj_t* s_img = nullptr;
static lv_obj_t* s_boxes[FACE_MAX_BOXES];
static lv_obj_t* s_arc = nullptr;
static lv_obj_t* s_count_lbl = nullptr;
static lv_obj_t* s_pill = nullptr;
static lv_obj_t* s_status_lbl = nullptr;

static lv_image_dsc_t s_dsc;
static uint8_t* s_buf = nullptr;  // preview RGB565, reused across verifies

// How long the captured shot stays on screen so the student can see what was
// saved, and how long the "shutter" whitening takes to fade out.
static constexpr int64_t FREEZE_US = 1000 * 1000;  // 1 s
static constexpr uint32_t FLASH_MS = 280;

static void flash_opa_cb(void* obj, int32_t v);  // defined with flash(), below
static lv_obj_t* s_flash = nullptr;   // white sheet, faded out after the shot
static int64_t s_freeze_until_us = 0;  // >0 while holding the captured frame

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
    if (s_flash) {
        // Kill the fade before its object goes away — an animation outliving its
        // target would tick on freed memory.
        lv_anim_delete(s_flash, flash_opa_cb);
        s_flash = nullptr;
    }
    if (s_overlay) {
        lv_obj_delete(s_overlay);  // deletes s_flash too (it is a child)
        s_overlay = nullptr;
    }
    s_freeze_until_us = 0;
    face_verify_done_cb cb = s_done;
    s_done = nullptr;
    return cb;
}

static void finish(bool verified) {
    face_verify_done_cb cb = take_down();
    if (cb) cb(verified);
}

// Animation target: the white sheet's background opacity.
static void flash_opa_cb(void* obj, int32_t v) {
    lv_obj_set_style_bg_opa((lv_obj_t*)obj, (lv_opa_t)v, 0);
}

// Camera-shutter cue: snap the sheet to a partial white, then fade it out over
// the frozen frame. Partial (not full) white so the photo underneath stays
// visible through the flash — this is a cue, not a blackout.
static void flash(void) {
    if (!s_flash) return;
    lv_obj_remove_flag(s_flash, LV_OBJ_FLAG_HIDDEN);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_flash);
    lv_anim_set_exec_cb(&a, flash_opa_cb);
    lv_anim_set_values(&a, LV_OPA_80, LV_OPA_TRANSP);
    lv_anim_set_duration(&a, FLASH_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

static void verify_tick(lv_timer_t*) {
    // Holding the captured shot: deliberately do NOT pull a new frame, so s_buf
    // (and therefore what's on screen) stays the exact image that was saved.
    // The tap is registered when the hold ends.
    if (s_freeze_until_us) {
        if (esp_timer_get_time() >= s_freeze_until_us) finish(true);
        return;
    }

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
    int64_t left_us = s_timeout_us - (now - s_start_us);
    if (left_us < 0) left_us = 0;
    int remain = (int)((left_us + 999999) / 1000000);       // seconds, rounded up
    lv_arc_set_value(s_arc, (int32_t)(left_us / 1000));      // ms remaining -> ring

    if (n >= 1) {
        // Face seen. Stop refreshing the preview FIRST: s_buf now holds exactly
        // the frame we are about to save, so the frozen image on screen is the
        // photo itself, not a later one.
        char path[96];
        bool saved = false;
        if (checkin_store_next_path(s_id, s_date, s_code, path, sizeof(path))) {
            saved = photo_store_encode_to(path, s_buf, FACE_PREVIEW_W, FACE_PREVIEW_H);
            if (!saved) ESP_LOGE(TAG, "check-in photo NOT saved: %s", path);
        } else {
            ESP_LOGE(TAG, "could not build a check-in photo path for student %s", s_id);
        }

        lv_label_set_text(s_status_lbl, saved ? LV_SYMBOL_OK "  Photo saved"
                                              : LV_SYMBOL_WARNING "  Photo not saved");
        lv_obj_set_style_text_color(s_status_lbl,
                                    lv_color_hex(saved ? THEME_DARK_OK : THEME_DARK_WARN), 0);
        lv_obj_set_style_bg_color(s_pill, lv_color_hex(saved ? THEME_DARK_OK : THEME_DARK_WARN),
                                  0);
        lv_obj_set_style_bg_opa(s_pill, LV_OPA_20, 0);
        lv_obj_set_style_arc_color(s_arc, lv_color_hex(THEME_DARK_OK), LV_PART_INDICATOR);
        lv_label_set_text(s_count_lbl, LV_SYMBOL_OK);  // ring stops counting down

        // Shutter: whiten the screen, then fade it out over the held frame. The
        // attendance tap is registered when the hold ends (see the branch above),
        // so the student always sees the shot that was taken.
        flash();
        s_freeze_until_us = esp_timer_get_time() + FREEZE_US;
        return;
    }

    char c[8];
    snprintf(c, sizeof(c), "%d", remain);
    lv_label_set_text(s_count_lbl, c);
    if (now - s_start_us >= s_timeout_us) {
        finish(false);
    }
}

// Small round avatar (reference photo if present, else a placeholder) shown for
// context so the student sees whose attendance is being verified.
static void verify_avatar(lv_obj_t* parent, const char* student_id) {
    // Deliberately small: this is a thumbnail beside the name for context, not
    // the hero avatar the check-in and kiosk panels show at AVATAR_MAX_PX.
    const int SZ = 64;
    // The photo used to be drawn unsized here, so a real avatar came out at its
    // full source size while the placeholder was SZ — fitting it to the same
    // box makes the two agree and gives clip_corner a square to round.
    lv_obj_t* img = student_photo_image(parent, student_id, SZ);
    if (img) {
        lv_obj_set_style_radius(img, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_clip_corner(img, true, 0);
        return;
    }
    lv_obj_t* ph = lv_obj_create(parent);
    lv_obj_remove_flag(ph, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(ph, SZ, SZ);
    lv_obj_set_style_radius(ph, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(ph, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(ph, LV_OPA_10, 0);
    lv_obj_set_style_border_width(ph, 0, 0);
    lv_obj_t* g = lv_label_create(ph);
    lv_label_set_text(g, LV_SYMBOL_IMAGE);
    lv_obj_set_style_text_color(g, lv_color_hex(THEME_DARK_MUTED), 0);
    lv_obj_center(g);
}

void face_verify_open(const char* student_id, const char* student_name, const char* date,
                      const char* class_code, int timeout_s, face_verify_done_cb done) {
    // NEVER return from this function without resolving `done`. The kiosk
    // disarms its card capture *before* calling us and re-arms only from the
    // callback, so a silent bail leaves the kiosk deaf to every future tap —
    // while the RFID task keeps logging "RFID card UID: ..." as if all is well.
    // This is what stranded a device in kiosk mode with dead input.
    if (s_overlay) {
        // Shouldn't happen (one verify at a time), but if a previous overlay was
        // somehow orphaned it is a full-screen CLICKABLE object on lv_layer_top()
        // — a GLOBAL layer that survives screen changes — so it would swallow
        // every touch, everywhere, forever. Tear it down and continue.
        ESP_LOGW(TAG, "verify overlay already open — force-closing the stale one");
        face_verify_done_cb stale = take_down();
        if (stale) stale(false);  // let the stranded caller finish its flow
    }
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
    s_freeze_until_us = 0;
    s_start_us = esp_timer_get_time();
    s_timeout_us = (int64_t)(timeout_s > 0 ? timeout_s : 1) * 1000000;

    // Full-bleed overlay: viewfinder pinned to the top, a controls block filling
    // (and centered in) the remaining height of the tall portrait screen.
    s_overlay = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_overlay);
    lv_obj_set_size(s_overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_overlay, lv_color_hex(THEME_DARK_BG_DEEP), 0);
    lv_obj_set_style_bg_opa(s_overlay, LV_OPA_COVER, 0);
    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(s_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(s_overlay, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_overlay, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(s_overlay, 0, 0);
    lv_obj_set_style_pad_row(s_overlay, 0, 0);

    // Live preview at native size so the face-box coordinates map 1:1. Rounded +
    // clipped for a framed "viewfinder" look; the top corners blend into the bg.
    lv_obj_t* holder = lv_obj_create(s_overlay);
    lv_obj_remove_style_all(holder);
    lv_obj_set_size(holder, FACE_PREVIEW_W, FACE_PREVIEW_H);
    lv_obj_remove_flag(holder, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(holder, 18, 0);
    lv_obj_set_style_clip_corner(holder, true, 0);
    lv_obj_set_style_border_width(holder, 1, 0);
    lv_obj_set_style_border_color(holder, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_opa(holder, LV_OPA_10, 0);

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

    // Controls block: grows to fill the space under the viewfinder, its content
    // vertically centered so the tall portrait screen doesn't read as top-heavy.
    lv_obj_t* body = lv_obj_create(s_overlay);
    lv_obj_remove_style_all(body);
    lv_obj_set_width(body, LV_PCT(100));
    lv_obj_set_flex_grow(body, 1);
    lv_obj_remove_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(body, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(body, 24, 0);
    lv_obj_set_style_pad_row(body, 16, 0);

    lv_obj_t* title = lv_label_create(body);
    lv_label_set_text(title, "Show your face to the camera");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(THEME_DARK_TEXT), 0);

    // Avatar + name, side by side, for context on whose attendance this is.
    lv_obj_t* who = lv_obj_create(body);
    lv_obj_remove_style_all(who);
    // Full width, not content: the name below takes what the avatar leaves, and
    // a content-sized row would have nothing to divide.
    lv_obj_set_size(who, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_remove_flag(who, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(who, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(who, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(who, 12, 0);
    verify_avatar(who, student_id);
    lv_obj_t* namel = lv_label_create(who);
    lv_label_set_text(namel, student_name && student_name[0] ? student_name : "");
    lv_obj_set_style_text_font(namel, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(namel, lv_color_hex(THEME_DARK_TEXT), 0);
    ui_label_fit(namel);

    // Countdown ring with the remaining seconds in its center.
    const int RING = 132;
    s_arc = lv_arc_create(body);
    lv_obj_set_size(s_arc, RING, RING);
    lv_arc_set_rotation(s_arc, 270);
    lv_arc_set_bg_angles(s_arc, 0, 360);
    lv_arc_set_range(s_arc, 0, (int32_t)(s_timeout_us / 1000));
    lv_arc_set_value(s_arc, (int32_t)(s_timeout_us / 1000));
    lv_obj_remove_flag(s_arc, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
    lv_obj_set_style_arc_width(s_arc, 8, LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_arc, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_arc_opa(s_arc, LV_OPA_10, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_arc, 8, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_arc, lv_color_hex(THEME_DARK_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s_arc, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_set_style_pad_all(s_arc, 0, LV_PART_KNOB);

    s_count_lbl = lv_label_create(s_arc);
    lv_label_set_text(s_count_lbl, "");
    lv_obj_set_style_text_font(s_count_lbl, &lv_font_montserrat_32, 0);
    lv_obj_set_style_text_color(s_count_lbl, lv_color_hex(THEME_DARK_TEXT), 0);
    lv_obj_center(s_count_lbl);

    // Status pill: neutral while searching, green with a check on detect.
    s_pill = lv_obj_create(body);
    lv_obj_remove_flag(s_pill, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s_pill, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(s_pill, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_pill, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(s_pill, LV_OPA_10, 0);
    lv_obj_set_style_border_width(s_pill, 0, 0);
    lv_obj_set_style_pad_hor(s_pill, 16, 0);
    lv_obj_set_style_pad_ver(s_pill, 8, 0);
    s_status_lbl = lv_label_create(s_pill);
    lv_label_set_text(s_status_lbl, "Looking for a face...");
    lv_obj_set_style_text_font(s_status_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_status_lbl, lv_color_hex(THEME_DARK_MUTED), 0);

    // Shutter sheet, created last so it sits above everything in the overlay.
    // IGNORE_LAYOUT is required: the overlay is a flex column, so without it the
    // sheet would be laid out as another child and shove the content around.
    s_flash = lv_obj_create(s_overlay);
    lv_obj_remove_style_all(s_flash);
    lv_obj_add_flag(s_flash, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_remove_flag(s_flash, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
    lv_obj_set_size(s_flash, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(s_flash, 0, 0);
    lv_obj_set_style_bg_color(s_flash, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(s_flash, LV_OPA_TRANSP, 0);
    lv_obj_add_flag(s_flash, LV_OBJ_FLAG_HIDDEN);

    s_timer = lv_timer_create(verify_tick, 100, nullptr);

    // The overlay is the heaviest thing this app builds in one go (preview image
    // + boxes + arc + avatar decode). If the LVGL heap runs dry mid-build, LVGL
    // asserts and halts the thread silently, so log the headroom left: a small
    // free_size here is the warning sign before a freeze.
    lv_mem_monitor_t m;
    lv_mem_monitor(&m);
    ESP_LOGI(TAG, "verify overlay up for %s — LVGL heap free %u/%u B (%u%% used, max blk %u)",
             s_id, (unsigned)m.free_size, (unsigned)m.total_size, (unsigned)m.used_pct,
             (unsigned)m.free_biggest_size);
}

void face_verify_cancel(void) { take_down(); }
