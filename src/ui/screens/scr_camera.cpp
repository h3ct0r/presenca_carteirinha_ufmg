#include "ui/screens/scr_camera.h"

#include "esp_heap_caps.h"
#include "services/face_detection_service.h"
#include "storage/photo_store.h"
#include "ui/components/shell.h"
#include "ui/screen_manager.h"
#include "ui/theme/theme.h"

static shell_t s_sh;
static lv_obj_t* s_img = nullptr;
static lv_obj_t* s_boxes[FACE_MAX_BOXES];
static lv_obj_t* s_status = nullptr;
static lv_obj_t* s_models = nullptr;       // which models are loaded + their paths
static lv_obj_t* s_saved_modal = nullptr;  // "picture saved" popup
static lv_timer_t* s_timer = nullptr;      // polls the service while visible
static lv_image_dsc_t s_dsc;
static uint8_t* s_buf = nullptr;           // preview RGB565, owned by this screen
static uint32_t s_last_seq = 0;            // last photo_store save seq we showed

static void back_cb(lv_event_t*) { scr_mgr_show(SCREEN_ADMIN, nullptr); }
static void snap_cb(lv_event_t*) { face_detection_request_capture(); }

static void close_saved_modal(lv_event_t*) {
    if (s_saved_modal) {
        lv_obj_delete(s_saved_modal);
        s_saved_modal = nullptr;
    }
}

// Popup confirming a snapshot was written, showing its SD path. Mirrors the
// modal pattern used elsewhere (class unlock, admin password): a dimmed
// full-screen overlay outside the shell's flex flow, with a centered card.
static void show_saved_modal(const char* path) {
    close_saved_modal(nullptr);

    s_saved_modal = lv_obj_create(s_sh.root);
    lv_obj_remove_style_all(s_saved_modal);
    lv_obj_add_flag(s_saved_modal, LV_OBJ_FLAG_IGNORE_LAYOUT);  // true overlay
    lv_obj_set_size(s_saved_modal, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_saved_modal, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_saved_modal, LV_OPA_50, 0);
    lv_obj_add_flag(s_saved_modal, LV_OBJ_FLAG_CLICKABLE);  // swallow taps behind
    lv_obj_remove_flag(s_saved_modal, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* card = ui_make_card(s_saved_modal);
    lv_obj_set_width(card, LV_PCT(88));
    lv_obj_align(card, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 12, 0);

    ui_make_label(card, LV_SYMBOL_OK "  Picture saved", THEME_PRIMARY,
                  &lv_font_montserrat_20);
    lv_obj_t* p = ui_make_label(card, path, THEME_TEXT, &lv_font_montserrat_14);
    lv_label_set_long_mode(p, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(p, LV_PCT(100));

    lv_obj_t* ok = ui_make_button(card, "OK", &theme_style_btn_primary,
                                  close_saved_modal, nullptr);
    lv_obj_set_width(ok, LV_PCT(100));
}

// Pulls the latest frame + boxes + status from the service (all LVGL calls here
// stay on the LVGL thread).
static void refresh_cb(lv_timer_t*) {
    face_box_t boxes[FACE_MAX_BOXES];
    int n = face_detection_snapshot(s_buf, boxes, FACE_MAX_BOXES);

    char st[64];
    face_detection_status(st, sizeof(st));
    lv_label_set_text(s_status, st);

    char mi[224];
    face_detection_model_info(mi, sizeof(mi));
    lv_label_set_text(s_models, mi);

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

    // A new snapshot landed on the card — show its path.
    char path[64];
    uint32_t seq = photo_store_last_saved(path, sizeof(path));
    if (seq != s_last_seq) {
        s_last_seq = seq;
        show_saved_modal(path);
    }
}

static lv_obj_t* create(void) {
    s_buf = (uint8_t*)heap_caps_aligned_calloc(128, 1, FACE_PREVIEW_W * FACE_PREVIEW_H * 2,
                                               MALLOC_CAP_SPIRAM);

    s_sh = shell_create("Camera", "Face detection preview", false);
    shell_set_back(&s_sh, back_cb);

    // Let the native-width preview span edge to edge; its face-box coordinates
    // map 1:1 only at native size, so we don't scale it.
    lv_obj_set_style_pad_all(s_sh.body, 0, 0);
    lv_obj_set_style_pad_row(s_sh.body, 0, 0);

    // Preview stage sized exactly to the preview so box coords map 1:1.
    lv_obj_t* holder = lv_obj_create(s_sh.body);
    lv_obj_remove_style_all(holder);
    lv_obj_set_size(holder, FACE_PREVIEW_W, FACE_PREVIEW_H);
    lv_obj_set_style_bg_color(holder, lv_color_hex(THEME_DARK_BG), 0);
    lv_obj_set_style_bg_opa(holder, LV_OPA_COVER, 0);
    lv_obj_remove_flag(holder, LV_OBJ_FLAG_SCROLLABLE);

    s_img = lv_image_create(holder);
    lv_obj_set_pos(s_img, 0, 0);
    // Only bind the descriptor when the buffer actually allocated — pointing an
    // lv_image at NULL data crashes the renderer. If PSRAM is exhausted the
    // preview stays blank and the status line (set in on_show) explains why.
    if (s_buf) {
        s_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
        s_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
        s_dsc.header.w = FACE_PREVIEW_W;
        s_dsc.header.h = FACE_PREVIEW_H;
        s_dsc.header.stride = FACE_PREVIEW_W * 2;
        s_dsc.data_size = FACE_PREVIEW_W * FACE_PREVIEW_H * 2;
        s_dsc.data = s_buf;
        lv_image_set_src(s_img, &s_dsc);
    }

    for (int i = 0; i < FACE_MAX_BOXES; i++) {
        lv_obj_t* b = lv_obj_create(holder);
        lv_obj_set_style_bg_opa(b, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_color(b, lv_color_hex(0xFF3030), 0);
        lv_obj_set_style_border_width(b, 3, 0);
        lv_obj_set_style_radius(b, 4, 0);
        lv_obj_remove_flag(b, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
        lv_obj_add_flag(b, LV_OBJ_FLAG_HIDDEN);
        s_boxes[i] = b;
    }

    // Controls below the preview, in a padded column matching other screens.
    lv_obj_t* ctl = lv_obj_create(s_sh.body);
    lv_obj_remove_style_all(ctl);
    lv_obj_set_width(ctl, LV_PCT(100));
    lv_obj_set_flex_grow(ctl, 1);
    lv_obj_set_flex_flow(ctl, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(ctl, 12, 0);
    lv_obj_set_style_pad_row(ctl, 10, 0);
    lv_obj_set_scroll_dir(ctl, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(ctl, LV_SCROLLBAR_MODE_AUTO);

    lv_obj_t* status_card = ui_make_card(ctl);
    s_status = ui_make_label(status_card, "Starting camera...", THEME_TEXT,
                             &lv_font_montserrat_14);
    lv_label_set_long_mode(s_status, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_status, LV_PCT(100));

    lv_obj_t* snap = ui_make_button(ctl, LV_SYMBOL_IMAGE "  Take picture",
                                    &theme_style_btn_primary, snap_cb, nullptr);
    lv_obj_set_width(snap, LV_PCT(100));

    // Model diagnostic panel: paths, sizes, and load state.
    lv_obj_t* model_card = ui_make_card(ctl);
    s_models = ui_make_label(model_card, "checking models...", THEME_MUTED,
                             &lv_font_montserrat_14);
    lv_label_set_long_mode(s_models, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_models, LV_PCT(100));
    lv_obj_set_style_text_align(s_models, LV_TEXT_ALIGN_LEFT, 0);

    return s_sh.root;
}

static void on_show(void*) {
    if (!face_detection_start()) {
        lv_label_set_text(s_status, "Camera unavailable - check the connection / serial log");
    }
    // Sync to the current save count so a photo taken earlier doesn't re-pop.
    s_last_seq = photo_store_last_saved(nullptr, 0);
    if (!s_timer) s_timer = lv_timer_create(refresh_cb, 100, nullptr);
}

static void on_hide(void) {
    if (s_timer) {
        lv_timer_delete(s_timer);
        s_timer = nullptr;
    }
    close_saved_modal(nullptr);
}

const screen_t scr_camera = {
    .create = create,
    .on_show = on_show,
    .on_hide = on_hide,
};
