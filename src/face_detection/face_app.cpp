#include "face_detection/face_app.h"

#include <lvgl.h>

#include <list>

#include "camera/auto_exposure.h"
#include "camera/csi_pipeline.h"
#include "camera/ov02c10_camera.h"
#include "driver/ppa.h"
#include "esp32-hal-log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "human_face_detect.hpp"
#include "storage/photo_store.h"

static const char* TAG = "face_app";

// Camera native resolution and the downscaled preview shown in the tab
#define CAM_W ov02c10_camera::WIDTH
#define CAM_H ov02c10_camera::HEIGHT
#define PREVIEW_W 480
#define PREVIEW_H 270
#define MAX_FACES 5

static ov02c10_camera s_sensor(1);
static AutoExposure s_ae(s_sensor);

// Shared state between the detection task (producer) and LVGL timer (consumer)
struct face_box {
    int x, y, w, h;
    float score;
};
struct shared_state {
    SemaphoreHandle_t lock;
    uint8_t* preview[2];  // double buffer, RGB565 PREVIEW_W x PREVIEW_H
    int ready_idx;        // buffer with the newest complete frame (-1 = none)
    face_box boxes[MAX_FACES];
    int n_boxes;
    uint32_t frame_count;
    uint32_t infer_ms;
    char status[64];
    char rfid[40];  // last card UID as hex, written by the RFID task
};
static shared_state s_st;

// LVGL widgets
static lv_obj_t* s_img = NULL;
static lv_obj_t* s_boxes[MAX_FACES];
static lv_obj_t* s_label = NULL;
static lv_obj_t* s_photo_label = NULL;
static lv_obj_t* s_rfid_label = NULL;
static lv_image_dsc_t s_img_dsc;

// Set by the snapshot button (LVGL thread), consumed by the detection task,
// which owns full-res frame access and forwards the next frame to the SD
// writer via photo_store_capture.
static volatile bool s_capture_req = false;

static void detection_task(void* arg) {
    // PPA client for hardware downscale 1920x1080 -> 480x270
    ppa_client_handle_t ppa = NULL;
    ppa_client_config_t ppa_cfg = {};
    ppa_cfg.oper_type = PPA_OPERATION_SRM;
    ESP_ERROR_CHECK(ppa_register_client(&ppa_cfg, &ppa));

    snprintf(s_st.status, sizeof(s_st.status), "loading model...");
    HumanFaceDetect* detect = new HumanFaceDetect();
    detect->set_score_thr(0.3f, 0);   // stage 0 (MSR) — controls what gets proposed
    detect->set_score_thr(0.45f, 1);  // stage 1 (MNP) — controls final accept/reject

    snprintf(s_st.status, sizeof(s_st.status), "model ready");
    ESP_LOGI(TAG, "detector ready, entering capture loop");

    int write_idx = 0;
    while (true) {
        size_t len = 0;
        uint8_t* frame = csi_pipeline_get_frame(1000, &len);
        if (frame == nullptr) {
            snprintf(s_st.status, sizeof(s_st.status), "no frame (timeout)");
            continue;
        }

        // Snapshot requested from the UI: hand this full-res frame to the SD
        // writer (copies it and returns; the write happens off this task).
        if (s_capture_req) {
            s_capture_req = false;
            photo_store_capture(frame, CAM_W, CAM_H);
        }

        uint8_t* dst = s_st.preview[write_idx];
        ppa_srm_oper_config_t op = {};
        op.in.buffer = frame;
        op.in.pic_w = CAM_W;
        op.in.pic_h = CAM_H;
        op.in.block_w = CAM_W;
        op.in.block_h = CAM_H;
        op.in.block_offset_x = 0;
        op.in.block_offset_y = 0;
        op.in.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
        op.out.buffer = dst;
        op.out.buffer_size = PREVIEW_W * PREVIEW_H * 2;
        op.out.pic_w = PREVIEW_W;
        op.out.pic_h = PREVIEW_H;
        op.out.block_offset_x = 0;
        op.out.block_offset_y = 0;
        op.out.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
        op.rotation_angle = PPA_SRM_ROTATION_ANGLE_0;
        op.scale_x = (float)PREVIEW_W / CAM_W;
        op.scale_y = (float)PREVIEW_H / CAM_H;
        op.mode = PPA_TRANS_MODE_BLOCKING;
        if (ppa_do_scale_rotate_mirror(ppa, &op) != ESP_OK) {
            snprintf(s_st.status, sizeof(s_st.status), "PPA scale failed");
            continue;
        }

        // Drive software AE/AWB from the preview frame (throttled internally).
        int luma = s_ae.update(dst, PREVIEW_W, PREVIEW_H);

        // Detect on the preview-sized frame (plenty for faces at room distance).
        // The pixel format is self-discovered: try RGB565LE / RGB565BE /
        // manually-unpacked RGB888 in turn, and whichever format last detected
        // a face is tried first on later frames, so steady-state runs a single
        // inference per frame.
        static int fmt_first = 0;  // sticky index into the format order below
        static const char* fmt_names[3] = {"LE", "BE", "888"};
        static uint8_t* rgb888 =
            (uint8_t*)heap_caps_aligned_calloc(64, 1, PREVIEW_W * PREVIEW_H * 3, MALLOC_CAP_SPIRAM);

        int64_t t0 = esp_timer_get_time();
        std::list<dl::detect::result_t> results;
        const char* fmt_path = "none";
        for (int attempt = 0; attempt < 3; attempt++) {
            int fmt = (attempt == 0) ? fmt_first : (attempt - (attempt <= fmt_first ? 1 : 0));
            if (attempt > 0 && fmt == fmt_first) continue;
            if (fmt == 2) {
                if (rgb888 == NULL) continue;
                const uint16_t* p = (const uint16_t*)dst;
                for (int i = 0; i < PREVIEW_W * PREVIEW_H; i++) {
                    uint16_t v = p[i];
                    rgb888[i * 3 + 0] = (v >> 8) & 0xF8;
                    rgb888[i * 3 + 1] = (v >> 3) & 0xFC;
                    rgb888[i * 3 + 2] = (v << 3) & 0xF8;
                }
                dl::image::img_t img8 = {.data = rgb888,
                                         .width = PREVIEW_W,
                                         .height = PREVIEW_H,
                                         .pix_type = dl::image::DL_IMAGE_PIX_TYPE_RGB888};
                results = detect->run(img8);
            } else {
                dl::image::img_t img = {.data = dst,
                                        .width = PREVIEW_W,
                                        .height = PREVIEW_H,
                                        .pix_type = (fmt == 0)
                                                        ? dl::image::DL_IMAGE_PIX_TYPE_RGB565LE
                                                        : dl::image::DL_IMAGE_PIX_TYPE_RGB565BE};
                results = detect->run(img);
            }
            if (!results.empty()) {
                fmt_first = fmt;
                fmt_path = fmt_names[fmt];
                break;
            }
        }
        uint32_t infer_ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);

        xSemaphoreTake(s_st.lock, portMAX_DELAY);
        s_st.ready_idx = write_idx;
        s_st.n_boxes = 0;
        for (const auto& res : results) {
            if (s_st.n_boxes >= MAX_FACES) break;
            face_box& b = s_st.boxes[s_st.n_boxes++];
            b.x = res.box[0];
            b.y = res.box[1];
            b.w = res.box[2] - res.box[0];
            b.h = res.box[3] - res.box[1];
            b.score = res.score;
        }
        s_st.frame_count++;
        s_st.infer_ms = infer_ms;
        snprintf(s_st.status, sizeof(s_st.status), "frame %u  infer %ums  faces %d (%s)  luma %d",
                 (unsigned)s_st.frame_count, (unsigned)infer_ms, s_st.n_boxes, fmt_path, luma);
        xSemaphoreGive(s_st.lock);

        write_idx ^= 1;  // never scale into the buffer LVGL may be reading
    }
}

// LVGL-side refresh: runs in the LVGL context (loop task), no extra locking needed there
static void ui_refresh_cb(lv_timer_t* t) {
    xSemaphoreTake(s_st.lock, portMAX_DELAY);
    int idx = s_st.ready_idx;
    int n = s_st.n_boxes;
    face_box boxes[MAX_FACES];
    memcpy(boxes, s_st.boxes, sizeof(boxes));
    char status[sizeof(s_st.status)];
    memcpy(status, s_st.status, sizeof(status));
    char rfid[sizeof(s_st.rfid)];
    memcpy(rfid, s_st.rfid, sizeof(rfid));
    xSemaphoreGive(s_st.lock);

    static char last_rfid[sizeof(s_st.rfid)];
    if (rfid[0] != '\0' && strcmp(rfid, last_rfid) != 0) {
        memcpy(last_rfid, rfid, sizeof(last_rfid));
        lv_label_set_text(s_rfid_label, rfid);
    }

    lv_label_set_text(s_label, status);
    if (idx >= 0) {
        s_img_dsc.data = s_st.preview[idx];
        lv_image_set_src(s_img, &s_img_dsc);
        lv_obj_invalidate(s_img);
    }
    for (int i = 0; i < MAX_FACES; i++) {
        if (i < n) {
            lv_obj_set_pos(s_boxes[i], boxes[i].x, boxes[i].y);
            lv_obj_set_size(s_boxes[i], boxes[i].w, boxes[i].h);
            lv_obj_clear_flag(s_boxes[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_boxes[i], LV_OBJ_FLAG_HIDDEN);
        }
    }

    // SD/snapshot state lives on other tasks; poll it into the label here so
    // all LVGL calls stay on this thread.
    static char last_photo[48];
    char photo[48];
    photo_store_get_status(photo, sizeof(photo));
    if (strcmp(photo, last_photo) != 0) {
        memcpy(last_photo, photo, sizeof(last_photo));
        lv_label_set_text(s_photo_label, photo);
    }
}

static void snapshot_btn_cb(lv_event_t* e) {
    s_capture_req = true;
}

void face_app_notify_rfid(const uint8_t* uid, uint8_t uid_len) {
    if (s_st.lock == NULL || uid == NULL) return;

    char hex[40];
    int pos = snprintf(hex, sizeof(hex), "RFID: ");
    for (uint8_t i = 0; i < uid_len && pos < (int)sizeof(hex) - 3; i++) {
        pos += snprintf(hex + pos, sizeof(hex) - pos, "%02X%s", uid[i], i + 1 < uid_len ? ":" : "");
    }

    xSemaphoreTake(s_st.lock, portMAX_DELAY);
    memcpy(s_st.rfid, hex, sizeof(s_st.rfid));
    xSemaphoreGive(s_st.lock);
}

static void build_ui() {
    lv_obj_t* tv = lv_tabview_create(lv_screen_active());
    lv_tabview_set_tab_bar_size(tv, 40);

    // --- Face Detect tab ---
    lv_obj_t* tab_face = lv_tabview_add_tab(tv, "Face Detect");
    lv_obj_set_style_pad_all(tab_face, 0, 0);

    // Preview container is exactly preview-sized so box coords map 1:1
    lv_obj_t* holder = lv_obj_create(tab_face);
    lv_obj_set_size(holder, PREVIEW_W, PREVIEW_H);
    lv_obj_align(holder, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_pad_all(holder, 0, 0);
    lv_obj_set_style_border_width(holder, 0, 0);
    lv_obj_set_style_radius(holder, 0, 0);
    lv_obj_clear_flag(holder, LV_OBJ_FLAG_SCROLLABLE);

    s_img_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    s_img_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    s_img_dsc.header.w = PREVIEW_W;
    s_img_dsc.header.h = PREVIEW_H;
    s_img_dsc.header.stride = PREVIEW_W * 2;
    s_img_dsc.data_size = PREVIEW_W * PREVIEW_H * 2;
    s_img_dsc.data = s_st.preview[0];

    s_img = lv_image_create(holder);
    lv_obj_set_pos(s_img, 0, 0);
    lv_image_set_src(s_img, &s_img_dsc);

    for (int i = 0; i < MAX_FACES; i++) {
        lv_obj_t* box = lv_obj_create(holder);
        lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_color(box, lv_color_hex(0xFF3030), 0);
        lv_obj_set_style_border_width(box, 3, 0);
        lv_obj_set_style_radius(box, 4, 0);
        lv_obj_clear_flag(box, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
        lv_obj_add_flag(box, LV_OBJ_FLAG_HIDDEN);
        s_boxes[i] = box;
    }

    s_label = lv_label_create(tab_face);
    lv_label_set_text(s_label, "starting camera...");
    lv_obj_align(s_label, LV_ALIGN_TOP_MID, 0, PREVIEW_H + 10);

    // Snapshot button + SD status, below the feed/status area
    lv_obj_t* snap_btn = lv_button_create(tab_face);
    lv_obj_set_size(snap_btn, 220, 56);
    lv_obj_align(snap_btn, LV_ALIGN_TOP_MID, 0, PREVIEW_H + 44);
    lv_obj_add_event_cb(snap_btn, snapshot_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t* snap_label = lv_label_create(snap_btn);
    lv_label_set_text(snap_label, LV_SYMBOL_IMAGE "  Take Picture");
    lv_obj_center(snap_label);

    s_photo_label = lv_label_create(tab_face);
    lv_label_set_text(s_photo_label, "");
    lv_obj_align(s_photo_label, LV_ALIGN_TOP_MID, 0, PREVIEW_H + 112);

    s_rfid_label = lv_label_create(tab_face);
    lv_label_set_text(s_rfid_label, "RFID: waiting for card...");
    lv_obj_set_style_text_font(s_rfid_label, &lv_font_montserrat_20, 0);
    lv_obj_align(s_rfid_label, LV_ALIGN_TOP_MID, 0, PREVIEW_H + 148);

    // --- Widgets tab (small placeholder playground) ---
    lv_obj_t* tab_w = lv_tabview_add_tab(tv, "Widgets");
    lv_obj_set_flex_flow(tab_w, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(tab_w, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t* sw = lv_switch_create(tab_w);
    (void)sw;
    lv_obj_t* slider = lv_slider_create(tab_w);
    lv_obj_set_width(slider, 300);
    lv_obj_t* btn = lv_button_create(tab_w);
    lv_obj_t* bl = lv_label_create(btn);
    lv_label_set_text(bl, "Button");
}

void face_app_start() {
    s_st.lock = xSemaphoreCreateMutex();
    s_st.ready_idx = -1;
    for (int i = 0; i < 2; i++) {
        s_st.preview[i] = (uint8_t*)heap_caps_aligned_calloc(128, 1, PREVIEW_W * PREVIEW_H * 2,
                                                             MALLOC_CAP_SPIRAM);
        assert(s_st.preview[i]);
    }
    snprintf(s_st.status, sizeof(s_st.status), "starting camera...");

    // Mount the TF card before the UI exists so the first status poll is real
    photo_store_init();

    build_ui();
    lv_timer_create(ui_refresh_cb, 100, NULL);

    // Order: CSI RX must be running before the sensor starts streaming
    bool cam_ok = csi_pipeline_init(CAM_W, CAM_H) && s_sensor.begin() && s_sensor.stream(true);
    if (cam_ok) {
        // decent indoor starting point; software AE refines from here
        s_sensor.set_exposure(800);
        s_sensor.set_gain_index(0);
    }
    if (!cam_ok) {
        snprintf(s_st.status, sizeof(s_st.status), "camera init FAILED - check serial log");
        ESP_LOGE(TAG, "camera init failed; face tab shows error state");
        return;
    }

    // Detection needs headroom: its own task on core 0 (LVGL runs on core 1)
    xTaskCreatePinnedToCore(detection_task, "face_detect", 16384, NULL, 5, NULL, 0);
}
