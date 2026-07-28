#include "camera/csi_pipeline.h"

#include <math.h>

#include "driver/isp.h"
#include "esp32-hal-log.h"
#include "esp_cache.h"
#include "esp_cam_ctlr.h"
#include "esp_cam_ctlr_csi.h"
#include "esp_heap_caps.h"
#include "esp_ldo_regulator.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char* TAG = "csi_pipe";

// Bring-up must fail gracefully, not abort. ESP_ERROR_CHECK panics (and reboots
// the whole device) on any driver error, which on the camera screen shows as a
// blue panel and a reset. Instead, log the failing call and return false so
// face_detection_start() reports "camera init FAILED" and the UI stays alive.
#define CSI_TRY(call)                                            \
    do {                                                         \
        esp_err_t err_ = (call);                                 \
        if (err_ != ESP_OK) {                                    \
            ESP_LOGE(TAG, "%s failed: 0x%x", #call, err_);       \
            return false;                                        \
        }                                                        \
    } while (0)

static esp_cam_ctlr_handle_t s_cam = NULL;
static isp_proc_handle_t s_isp = NULL;
static uint8_t* s_buf[2] = {NULL, NULL};  // DMA ping-pong frame buffers
static size_t s_fb_len = 0;
static volatile int s_latest = -1;  // newest complete frame index
static volatile int s_dma_idx = 0;  // buffer DMA writes into next
static SemaphoreHandle_t s_frame_sem = NULL;

// Color correction matrix for the OV02C10, from the vendor ISP tuning JSON
// (vendor_idf_examples/.../cfg/ov02c10_default_p4_eco4.json, acc/ccm table)
static const float k_base_ccm[3][3] = {
    {1.408f, -0.094f, -0.314f},
    {-0.13f, 1.28f, -0.15f},
    {-0.072f, -0.173f, 1.245f},
};

// Vendor gamma tuning: out = in^0.518 (brightens midtones after the linear pipeline)
static uint32_t gamma_curve(uint32_t x) {
    return (uint32_t)(255.0f * powf((float)x / 255.0f, 0.518f) + 0.5f);
}

// White balance folds into the CCM: scaling the R/B input channels is the same
// as scaling the corresponding matrix columns
static void apply_ccm(float r_gain, float b_gain) {
    esp_isp_ccm_config_t ccm = {};
    ccm.saturation = true;
    for (int i = 0; i < 3; i++) {
        ccm.matrix[i][0] = k_base_ccm[i][0] * r_gain;
        ccm.matrix[i][1] = k_base_ccm[i][1];
        ccm.matrix[i][2] = k_base_ccm[i][2] * b_gain;
        for (int j = 0; j < 3; j++) {
            if (ccm.matrix[i][j] > 3.99f) ccm.matrix[i][j] = 3.99f;
            if (ccm.matrix[i][j] < -3.99f) ccm.matrix[i][j] = -3.99f;
        }
    }
    esp_isp_ccm_configure(s_isp, &ccm);
}

void csi_pipeline_set_wb(float r_gain, float b_gain) {
    if (s_isp != NULL) {
        apply_ccm(r_gain, b_gain);
    }
}

// Both callbacks run in ISR context: no logging, no blocking
static bool IRAM_ATTR on_get_new_trans(esp_cam_ctlr_handle_t handle, esp_cam_ctlr_trans_t* trans,
                                       void* user_data) {
    trans->buffer = s_buf[s_dma_idx];
    trans->buflen = s_fb_len;
    return false;
}

static bool IRAM_ATTR on_trans_finished(esp_cam_ctlr_handle_t handle, esp_cam_ctlr_trans_t* trans,
                                        void* user_data) {
    s_latest = (trans->buffer == s_buf[0]) ? 0 : 1;
    s_dma_idx = s_latest ^ 1;  // next DMA frame goes to the other buffer
    BaseType_t hp = pdFALSE;
    xSemaphoreGiveFromISR(s_frame_sem, &hp);
    return hp == pdTRUE;
}

bool csi_pipeline_init(uint16_t w, uint16_t h) {
    // MIPI PHY rail (LDO channel 3, 2.5V). The DSI display init usually owns it
    // already; a failure here just means it is already powered.
    esp_ldo_channel_config_t ldo_cfg = {
        .chan_id = 3,
        .voltage_mv = 2500,
    };
    esp_ldo_channel_handle_t ldo = NULL;
    if (esp_ldo_acquire_channel(&ldo_cfg, &ldo) != ESP_OK) {
        ESP_LOGI(TAG, "MIPI PHY LDO already powered (by display init)");
    }

    s_frame_sem = xSemaphoreCreateBinary();
    s_fb_len = (size_t)w * h * 2;  // RGB565
    for (int i = 0; i < 2; i++) {
        s_buf[i] = (uint8_t*)heap_caps_aligned_calloc(128, 1, s_fb_len, MALLOC_CAP_SPIRAM);
        if (s_buf[i] == NULL) {
            ESP_LOGE(TAG, "frame buffer alloc failed (%u bytes)", (unsigned)s_fb_len);
            return false;
        }
    }

    esp_cam_ctlr_csi_config_t csi_cfg = {};
    csi_cfg.ctlr_id = 0;
    csi_cfg.clk_src = MIPI_CSI_PHY_CLK_SRC_DEFAULT;
    csi_cfg.h_res = w;
    csi_cfg.v_res = h;
    csi_cfg.data_lane_num = 2;
    csi_cfg.lane_bit_rate_mbps = 810;  // merged driver: mipi_clk 405MHz -> 810Mbps/lane
    csi_cfg.input_data_color_type = CAM_CTLR_COLOR_RAW10;
    csi_cfg.output_data_color_type = CAM_CTLR_COLOR_RGB565;
    csi_cfg.queue_items = 2;
    esp_err_t err = esp_cam_new_csi_ctlr(&csi_cfg, &s_cam);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_cam_new_csi_ctlr failed: 0x%x", err);
        return false;
    }

    esp_cam_ctlr_evt_cbs_t cbs = {
        .on_get_new_trans = on_get_new_trans,
        .on_trans_finished = on_trans_finished,
    };
    CSI_TRY(esp_cam_ctlr_register_event_callbacks(s_cam, &cbs, NULL));

    esp_isp_processor_cfg_t isp_cfg = {};
    isp_cfg.clk_src = ISP_CLK_SRC_DEFAULT;
    isp_cfg.clk_hz = 120000000;
    isp_cfg.input_data_source = ISP_INPUT_DATA_SOURCE_CSI;
    isp_cfg.input_data_color_type = ISP_COLOR_RAW10;
    isp_cfg.output_data_color_type = ISP_COLOR_RGB565;
    isp_cfg.h_res = w;
    isp_cfg.v_res = h;
    isp_cfg.bayer_order = COLOR_RAW_ELEMENT_ORDER_GBRG;  // OV02C10 is GBRG
    err = esp_isp_new_processor(&isp_cfg, &s_isp);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_isp_new_processor failed: 0x%x", err);
        return false;
    }
    CSI_TRY(esp_isp_enable(s_isp));

    // Color pipeline: vendor CCM (neutral WB to start; AWB loop adjusts it)
    // and the vendor 0.518 gamma curve on all three channels
    apply_ccm(1.0f, 1.0f);
    CSI_TRY(esp_isp_ccm_enable(s_isp));

    isp_gamma_curve_points_t pts;
    CSI_TRY(esp_isp_gamma_fill_curve_points(gamma_curve, &pts));
    CSI_TRY(esp_isp_gamma_configure(s_isp, COLOR_COMPONENT_R, &pts));
    CSI_TRY(esp_isp_gamma_configure(s_isp, COLOR_COMPONENT_G, &pts));
    CSI_TRY(esp_isp_gamma_configure(s_isp, COLOR_COMPONENT_B, &pts));
    CSI_TRY(esp_isp_gamma_enable(s_isp));

    CSI_TRY(esp_cam_ctlr_enable(s_cam));
    CSI_TRY(esp_cam_ctlr_start(s_cam));

    ESP_LOGI(TAG, "CSI pipeline up: %ux%u RAW10(GBRG) -> RGB565, 2 lanes @810Mbps", w, h);
    return true;
}

uint8_t* csi_pipeline_get_frame(uint32_t timeout_ms, size_t* out_len) {
    if (s_cam == NULL || s_frame_sem == NULL) {
        return nullptr;
    }
    if (xSemaphoreTake(s_frame_sem, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        return nullptr;
    }
    int idx = s_latest;
    if (idx < 0) {
        return nullptr;
    }
    // DMA wrote PSRAM behind the cache: invalidate before the CPU reads
    esp_cache_msync(s_buf[idx], s_fb_len, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
    if (out_len != NULL) {
        *out_len = s_fb_len;
    }
    return s_buf[idx];
}
