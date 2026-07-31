#include "services/face_detection_service.h"

#include <SD_MMC.h>
#include <string.h>

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
#include "services/wifi_ap.h"
#include "storage/photo_store.h"

// Face inference is optional: it needs the ESP-DL framework (dl::*), which is a
// separate build dependency. Define USE_FACE_DETECT (and add esp-dl) to enable.
#ifdef USE_FACE_DETECT
#include <list>

#include "human_face_detect.hpp"
#endif

static const char* TAG = "face_svc";

// Loading a model reads it off the SD card, and sdmmc needs a DMA-capable
// INTERNAL buffer to do that — the one pool the WiFi stack permanently eats
// into. At 46,976 B free it failed with ESP_ERR_NO_MEM, esp-dl logged "Fail to
// load model" and then used the null model anyway (dl::Model::minimize() ->
// FbsModel::clear_map()), which panics the device. So this is the number to
// watch; PSRAM is never the constraint here.
static void log_heap(const char* when) {
    ESP_LOGI(TAG, "%s: internal %u B free (largest %u), DMA %u B, PSRAM %u B", when,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

#define CAM_W ov02c10_camera::WIDTH
#define CAM_H ov02c10_camera::HEIGHT
#define PREVIEW_BYTES (FACE_PREVIEW_W * FACE_PREVIEW_H * 2)

// SD_MMC-relative model paths (the card is VFS-mounted at /sdcard, so esp-dl
// opens them as /sdcard/models/..., which is the same file).
#define MODEL_MSR "/models/human_face_detect_msr_s8_v1.espdl"
#define MODEL_MNP "/models/human_face_detect_mnp_s8_v1.espdl"

static ov02c10_camera s_sensor(1);
static AutoExposure s_ae(s_sensor);

// Shared state: the detection task (producer) fills a back buffer and, under
// the lock, flips `ready_idx`; face_detection_snapshot() (consumer) copies it.
struct shared_state {
    SemaphoreHandle_t lock = nullptr;
    uint8_t* preview[2] = {nullptr, nullptr};  // double buffer, RGB565
    int ready_idx = -1;                        // buffer with the newest frame (-1 = none)
    face_box_t boxes[FACE_MAX_BOXES] = {};
    int n_boxes = 0;
    uint32_t frame_count = 0;
    char status[64] = "idle";
    char model_info[224] = "checking models...";  // paths + load state, set once at task start
};
static shared_state s_st;

static bool s_running = false;
// Pause (not teardown): the heavy CSI/ISP pipeline stays initialized after the
// one-time bring-up; stop just idles the detection task and turns the sensor
// stream off, and start resumes it. Avoids the risky deinit/re-bring-up path.
static volatile bool s_paused = false;
static volatile bool s_capture_req = false;
// Set by the detection task once it knows the model is not coming. Stays false
// while loading, so callers can't mistake "still loading" for "never".
static volatile bool s_model_unavailable = false;

static void set_status(const char* s) {
    if (!s_st.lock) return;
    xSemaphoreTake(s_st.lock, portMAX_DELAY);
    snprintf(s_st.status, sizeof(s_st.status), "%s", s);
    xSemaphoreGive(s_st.lock);
}

// Hardware downscale full-res RGB565 -> preview RGB565 via the PPA engine.
static bool downscale(ppa_client_handle_t ppa, const uint8_t* src, uint8_t* dst) {
    ppa_srm_oper_config_t op = {};
    op.in.buffer = src;
    op.in.pic_w = CAM_W;
    op.in.pic_h = CAM_H;
    op.in.block_w = CAM_W;
    op.in.block_h = CAM_H;
    op.in.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
    op.out.buffer = dst;
    op.out.buffer_size = PREVIEW_BYTES;
    op.out.pic_w = FACE_PREVIEW_W;
    op.out.pic_h = FACE_PREVIEW_H;
    op.out.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
    op.rotation_angle = PPA_SRM_ROTATION_ANGLE_0;
    op.scale_x = (float)FACE_PREVIEW_W / CAM_W;
    op.scale_y = (float)FACE_PREVIEW_H / CAM_H;
    op.mode = PPA_TRANS_MODE_BLOCKING;
    return ppa_do_scale_rotate_mirror(ppa, &op) == ESP_OK;
}

#ifdef USE_FACE_DETECT
static const char* FMT_NAMES[3] = {"565LE", "565BE", "888"};

// Runs the two-stage detector on the preview frame. The pixel format the model
// wants isn't known up front, so we try three interpretations — RGB565 little-
// and big-endian, and a manual RGB565->RGB888 unpack — and the one that last
// found a face is tried first (steady state = one inference per frame).
// `*out_fmt` reports the format that detected (or "none").
static int run_detection(HumanFaceDetect* detect, uint8_t* preview, face_box_t* out,
                         uint32_t* infer_ms, const char** out_fmt) {
    static int fmt_first = 0;  // sticky: 0=565LE, 1=565BE, 2=888
    static uint8_t* rgb888 = nullptr;
    if (!rgb888) {
        rgb888 = (uint8_t*)heap_caps_aligned_calloc(64, 1, FACE_PREVIEW_W * FACE_PREVIEW_H * 3,
                                                    MALLOC_CAP_SPIRAM);
    }

    // Try order: last-good format first, then the others.
    int order[3] = {fmt_first, 0, 0};
    for (int f = 0, k = 1; f < 3; f++) {
        if (f != fmt_first) order[k++] = f;
    }

    int64_t t0 = esp_timer_get_time();
    std::list<dl::detect::result_t> results;
    for (int oi = 0; oi < 3; oi++) {
        int fmt = order[oi];
        if (fmt == 2) {
            if (!rgb888) continue;
            const uint16_t* p = (const uint16_t*)preview;
            for (int i = 0; i < FACE_PREVIEW_W * FACE_PREVIEW_H; i++) {
                uint16_t v = p[i];
                rgb888[i * 3 + 0] = (v >> 8) & 0xF8;  // R
                rgb888[i * 3 + 1] = (v >> 3) & 0xFC;  // G
                rgb888[i * 3 + 2] = (v << 3) & 0xF8;  // B
            }
            dl::image::img_t img = {.data = rgb888,
                                    .width = FACE_PREVIEW_W,
                                    .height = FACE_PREVIEW_H,
                                    .pix_type = dl::image::DL_IMAGE_PIX_TYPE_RGB888};
            results = detect->run(img);
        } else {
            dl::image::img_t img = {.data = preview,
                                    .width = FACE_PREVIEW_W,
                                    .height = FACE_PREVIEW_H,
                                    .pix_type = fmt == 0 ? dl::image::DL_IMAGE_PIX_TYPE_RGB565LE
                                                         : dl::image::DL_IMAGE_PIX_TYPE_RGB565BE};
            results = detect->run(img);
        }
        if (!results.empty()) {
            fmt_first = fmt;
            break;
        }
    }
    *infer_ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
    if (out_fmt) *out_fmt = results.empty() ? "none" : FMT_NAMES[fmt_first];

    int n = 0;
    for (const auto& res : results) {
        if (n >= FACE_MAX_BOXES) break;
        out[n].x = res.box[0];
        out[n].y = res.box[1];
        out[n].w = res.box[2] - res.box[0];
        out[n].h = res.box[3] - res.box[1];
        out[n].score = res.score;
        n++;
    }
    return n;
}
#endif  // USE_FACE_DETECT

static void detection_task(void*) {
    ppa_client_handle_t ppa = nullptr;
    ppa_client_config_t ppa_cfg = {};
    ppa_cfg.oper_type = PPA_OPERATION_SRM;
    if (ppa_register_client(&ppa_cfg, &ppa) != ESP_OK) {
        set_status("PPA init failed");
        vTaskDelete(nullptr);
        return;
    }

#ifdef USE_FACE_DETECT
    // Probe the model files (size 0 / MISSING when absent or unreadable).
    bool have_msr = SD_MMC.exists(MODEL_MSR), have_mnp = SD_MMC.exists(MODEL_MNP);
    size_t sz_msr = 0, sz_mnp = 0;
    {
        File f = SD_MMC.open(MODEL_MSR);
        if (f) {
            sz_msr = f.size();
            f.close();
        }
        f = SD_MMC.open(MODEL_MNP);
        if (f) {
            sz_mnp = f.size();
            f.close();
        }
    }

    // esp-dl does NOT check its own model-load failure — it dereferences the
    // broken model and hard-faults (a device reset). So only construct the
    // detector when both model files are present AND non-empty; a 0-byte or
    // truncated file (a failed SD copy) is the common way this crashes.
    // NOTE: a genuinely corrupt but full-size model still faults inside esp-dl —
    // that is an esp-dl limitation we can't guard from here.
    static constexpr size_t MIN_MODEL_BYTES = 1024;  // any real model is far larger
    bool files_ok = have_msr && have_mnp && sz_msr >= MIN_MODEL_BYTES && sz_mnp >= MIN_MODEL_BYTES;

    // The other way the load fails is running out of DMA-capable internal RAM
    // for the SD read, which ends in the same unguardable esp-dl fault. Using
    // the debug WiFi AP is what causes it in practice: the stack it leaves
    // behind is never freed, not even by wifi_ap_stop() (see wifi_ap.h), so
    // once the AP has been up in this boot only a restart brings the memory
    // back. Refuse to load rather than panic.
    size_t internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    bool wifi_used = wifi_ap_was_started();
    // Calibrated from the observed failure: the SD read died at 46,976 B free.
    // Raise this if a load ever fails with more than that available.
    static constexpr size_t MIN_INTERNAL_FREE = 56 * 1024;
    bool memory_ok = !wifi_used && internal_free >= MIN_INTERNAL_FREE;

    HumanFaceDetect* detect = nullptr;
    if (files_ok && memory_ok) {
        set_status("loading model...");
        log_heap("before model load");
        // lazy_load = false: esp-dl otherwise defers the whole load to the first
        // frame with a face in it, so an out-of-memory panic would land minutes
        // later and look unrelated to opening the camera. Load it here, where
        // the guards above have just run.
        detect = new HumanFaceDetect(
            static_cast<HumanFaceDetect::model_type_t>(CONFIG_DEFAULT_HUMAN_FACE_DETECT_MODEL),
            false);
        detect->set_score_thr(0.3f, 0);   // stage 0 (MSR): proposals
        detect->set_score_thr(0.45f, 1);  // stage 1 (MNP): final accept
        log_heap("after model load");
        set_status("model ready");
    } else if (files_ok && wifi_used) {
        set_status("restart the device to use face detection (WiFi was used)");
        ESP_LOGW(TAG, "skipping model load: the WiFi AP ran this boot, %u B internal free",
                 (unsigned)internal_free);
    } else if (files_ok) {
        set_status("not enough memory for the model - preview only");
        ESP_LOGW(TAG, "skipping model load: %u B internal free, need %u",
                 (unsigned)internal_free, (unsigned)MIN_INTERNAL_FREE);
    } else if (have_msr && have_mnp) {
        set_status("model file empty/truncated - preview only");
        ESP_LOGW(TAG, "model file too small (msr=%uB mnp=%uB); preview only", (unsigned)sz_msr,
                 (unsigned)sz_mnp);
    } else {
        set_status("no model on SD (put .espdl in /models) - preview only");
        ESP_LOGW(TAG, "model files missing on SD (%s / %s); preview only", MODEL_MSR, MODEL_MNP);
    }
    s_model_unavailable = (detect == nullptr);

    xSemaphoreTake(s_st.lock, portMAX_DELAY);
    snprintf(s_st.model_info, sizeof(s_st.model_info),
             "MSR /sdcard%s\n    %s%u KB\nMNP /sdcard%s\n    %s%u KB\nDetector: %s", MODEL_MSR,
             have_msr ? "" : "MISSING  ", (unsigned)(sz_msr / 1024), MODEL_MNP,
             have_mnp ? "" : "MISSING  ", (unsigned)(sz_mnp / 1024),
             detect ? "LOADED" : "NOT loaded (preview only)");
    xSemaphoreGive(s_st.lock);
#else
    set_status("preview (detection disabled)");
    s_model_unavailable = true;
    xSemaphoreTake(s_st.lock, portMAX_DELAY);
    snprintf(s_st.model_info, sizeof(s_st.model_info), "detection not built in (USE_FACE_DETECT off)");
    xSemaphoreGive(s_st.lock);
#endif

    int write_idx = 0;
    face_box_t boxes[FACE_MAX_BOXES];
    while (true) {
        if (s_paused) {  // stopped: idle without touching the (streaming-off) pipeline
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        size_t len = 0;
        uint8_t* frame = csi_pipeline_get_frame(1000, &len);
        if (!frame) {
            set_status("no frame (timeout)");
            continue;
        }

        // Snapshot: hand the full-res frame to the SD writer (it copies).
        if (s_capture_req) {
            s_capture_req = false;
            photo_store_capture(frame, CAM_W, CAM_H);
        }

        uint8_t* dst = s_st.preview[write_idx];
        if (!downscale(ppa, frame, dst)) {
            set_status("PPA scale failed");
            continue;
        }
        int luma = s_ae.update(dst, FACE_PREVIEW_W, FACE_PREVIEW_H);

        int n = 0;
        uint32_t infer_ms = 0;
        const char* fmt = "-";
#ifdef USE_FACE_DETECT
        if (detect) n = run_detection(detect, dst, boxes, &infer_ms, &fmt);
#else
        (void)infer_ms;
#endif

        xSemaphoreTake(s_st.lock, portMAX_DELAY);
        s_st.ready_idx = write_idx;
        s_st.n_boxes = n;
        memcpy(s_st.boxes, boxes, n * sizeof(face_box_t));
        s_st.frame_count++;
        snprintf(s_st.status, sizeof(s_st.status),
                 "frame %u  faces %d [%s]  infer %ums  luma %d", (unsigned)s_st.frame_count, n,
                 fmt, (unsigned)infer_ms, luma);
        xSemaphoreGive(s_st.lock);

        write_idx ^= 1;  // never scale into the buffer the consumer may be copying
    }
}

bool face_detection_start(void) {
    log_heap("camera start");
    if (s_running) {  // already up: resume from a pause if needed
        if (s_paused) {
            s_sensor.stream(true);
            s_paused = false;
        }
        return true;
    }

    // Allocate the lock and preview buffers ONCE and reuse them across retries.
    // A camera-init failure returns early below with s_running still false, so a
    // re-entry into the screen would otherwise leak the mutex and 2x253 KB of
    // PSRAM every time, eventually exhausting PSRAM and crashing elsewhere.
    if (!s_st.lock) {
        s_st.lock = xSemaphoreCreateMutex();
        if (!s_st.lock) return false;
    }
    for (int i = 0; i < 2; i++) {
        if (s_st.preview[i]) continue;
        s_st.preview[i] =
            (uint8_t*)heap_caps_aligned_calloc(128, 1, PREVIEW_BYTES, MALLOC_CAP_SPIRAM);
        if (!s_st.preview[i]) {
            set_status("out of PSRAM");
            return false;
        }
    }
    photo_store_init();  // mount TF card for snapshots (idempotent)

    // CSI RX must run before the sensor streams.
    bool ok = csi_pipeline_init(CAM_W, CAM_H) && s_sensor.begin() && s_sensor.stream(true);
    if (!ok) {
        set_status("camera init FAILED");
        ESP_LOGE(TAG, "camera init failed");
        return false;
    }
    s_sensor.set_exposure(800);  // indoor starting point; software AE refines
    s_sensor.set_gain_index(0);

    // Detection is heavy: its own task pinned to core 0 (LVGL runs on core 1).
    xTaskCreatePinnedToCore(detection_task, "face_detect", 16384, nullptr, 5, nullptr, 0);
    s_running = true;
    ESP_LOGI(TAG, "camera + detection started");
    return true;
}

bool face_detection_running(void) { return s_running && !s_paused; }

bool face_detection_model_unavailable(void) { return s_model_unavailable; }

void face_detection_stop(void) {
    if (!s_running || s_paused) return;
    s_paused = true;              // the detection task idles on its next loop
    s_sensor.stream(false);       // sensor stops emitting frames
    set_status("camera paused");
    ESP_LOGI(TAG, "camera paused (pipeline kept warm)");
}

int face_detection_snapshot(uint8_t* dst, face_box_t* boxes, int max_boxes) {
    if (!s_st.lock || !dst) return -1;
    int n = -1;
    xSemaphoreTake(s_st.lock, portMAX_DELAY);
    if (s_st.ready_idx >= 0) {
        memcpy(dst, s_st.preview[s_st.ready_idx], PREVIEW_BYTES);
        n = s_st.n_boxes < max_boxes ? s_st.n_boxes : max_boxes;
        if (boxes) memcpy(boxes, s_st.boxes, n * sizeof(face_box_t));
    }
    xSemaphoreGive(s_st.lock);
    return n;
}

void face_detection_status(char* out, size_t cap) {
    if (!out || !cap) return;
    out[0] = '\0';
    if (!s_st.lock) return;
    xSemaphoreTake(s_st.lock, portMAX_DELAY);
    snprintf(out, cap, "%s", s_st.status);
    xSemaphoreGive(s_st.lock);
}

void face_detection_model_info(char* out, size_t cap) {
    if (!out || !cap) return;
    out[0] = '\0';
    if (!s_st.lock) return;
    xSemaphoreTake(s_st.lock, portMAX_DELAY);
    snprintf(out, cap, "%s", s_st.model_info);
    xSemaphoreGive(s_st.lock);
}

void face_detection_request_capture(void) { s_capture_req = true; }
