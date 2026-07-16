#include "storage/photo_store.h"

#include <FS.h>
#include <SD_MMC.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "driver/jpeg_encode.h"
#include "esp32-hal-log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char* TAG = "photo";

// The board variant (esp32p4) routes SD_MMC to SDMMC slot 0 IOMUX pins
// (GPIO39-44, the TF card on the schematic) and powers TF_VCC through the
// chip's LDO channel 4, so SD_MMC.begin() needs no pin setup here.

static SemaphoreHandle_t s_lock = NULL;  // guards s_status
static TaskHandle_t s_writer = NULL;
static char s_status[48] = "SD not mounted";
static bool s_mounted = false;
static volatile bool s_busy = false;

// Frame handed from photo_store_capture to the writer task
static uint8_t* s_frame = NULL;  // RGB565 copy (JPEG-DMA-aligned)
static uint8_t* s_bgr = NULL;    // BGR888 conversion buffer (BMP fallback)
static int s_w = 0, s_h = 0;
static int s_next_idx = 1;

// P4 hardware JPEG encoder: consumes the RGB565 frame directly, no CPU
// pixel conversion. NULL if engine creation failed (BMP fallback then).
static jpeg_encoder_handle_t s_jpeg = NULL;
static uint8_t* s_jpg_out = NULL;
static size_t s_jpg_out_cap = 0;

static void set_status(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    vsnprintf(s_status, sizeof(s_status), fmt, ap);
    xSemaphoreGive(s_lock);
    va_end(ap);
}

void photo_store_get_status(char* out, size_t out_len) {
    if (out == NULL || out_len == 0) return;
    if (s_lock == NULL) {
        snprintf(out, out_len, "%s", s_status);
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    snprintf(out, out_len, "%s", s_status);
    xSemaphoreGive(s_lock);
}

static void wr32(uint8_t* p, uint32_t v) {
    p[0] = v & 0xFF;
    p[1] = (v >> 8) & 0xFF;
    p[2] = (v >> 16) & 0xFF;
    p[3] = (v >> 24) & 0xFF;
}

// Hardware-encodes the pending RGB565 frame and writes IMG_nnnn.jpg.
// Returns false if the engine is missing or any step fails (caller falls
// back to the uncompressed BMP path).
static bool save_jpeg() {
    if (s_jpeg == NULL || s_frame == NULL) return false;

    if (s_jpg_out == NULL) {
        // Generous ceiling: quality-85 4:2:0 1080p lands around 300-500 KB
        jpeg_encode_memory_alloc_cfg_t mc = {.buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER};
        s_jpg_out = (uint8_t*)jpeg_alloc_encoder_mem((size_t)s_w * s_h, &mc, &s_jpg_out_cap);
    }
    if (s_jpg_out == NULL) return false;

    jpeg_encode_cfg_t cfg = {};
    cfg.width = (uint32_t)s_w;
    cfg.height = (uint32_t)s_h;
    cfg.src_type = JPEG_ENCODE_IN_FORMAT_RGB565;
    cfg.sub_sample = JPEG_DOWN_SAMPLING_YUV420;
    cfg.image_quality = 85;

    uint32_t jpg_size = 0;
    esp_err_t err = jpeg_encoder_process(s_jpeg, &cfg, s_frame, (uint32_t)s_w * s_h * 2, s_jpg_out,
                                         (uint32_t)s_jpg_out_cap, &jpg_size);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "hw jpeg encode failed: 0x%x", err);
        return false;
    }

    char name[32];
    snprintf(name, sizeof(name), "/photos/IMG_%04d.jpg", s_next_idx);
    File f = SD_MMC.open(name, FILE_WRITE, true);
    if (!f) {
        set_status("SD open failed");
        ESP_LOGE(TAG, "open %s failed", name);
        return false;
    }
    bool ok = f.write(s_jpg_out, jpg_size) == jpg_size;
    f.close();
    if (!ok) {
        set_status("SD write failed");
        ESP_LOGE(TAG, "write %s failed", name);
        return false;
    }

    set_status("saved IMG_%04d.jpg (%u KB)", s_next_idx, (unsigned)(jpg_size / 1024));
    ESP_LOGI(TAG, "saved %s (%u bytes)", name, (unsigned)jpg_size);
    s_next_idx++;
    return true;
}

static void save_bmp() {
    const int w = s_w, h = s_h;
    const size_t row = ((size_t)w * 3 + 3) & ~(size_t)3;  // rows pad to 4 bytes
    const uint32_t img_size = (uint32_t)(row * h);
    const uint32_t file_size = 54 + img_size;

    if (s_bgr == NULL) {
        s_bgr = (uint8_t*)heap_caps_calloc(1, img_size, MALLOC_CAP_SPIRAM);
    }
    if (s_bgr == NULL) {
        set_status("out of memory");
        return;
    }

    // RGB565 (same interpretation the preview/detector uses) -> BGR888,
    // bottom-up row order as BMP requires.
    const uint16_t* src = (const uint16_t*)s_frame;
    for (int y = 0; y < h; y++) {
        uint8_t* dst = s_bgr + row * (size_t)(h - 1 - y);
        const uint16_t* sp = src + (size_t)y * w;
        for (int x = 0; x < w; x++) {
            uint16_t v = sp[x];
            dst[x * 3 + 0] = (v << 3) & 0xF8;  // B
            dst[x * 3 + 1] = (v >> 3) & 0xFC;  // G
            dst[x * 3 + 2] = (v >> 8) & 0xF8;  // R
        }
    }

    // BITMAPFILEHEADER + BITMAPINFOHEADER, 24bpp uncompressed
    uint8_t hdr[54] = {0};
    hdr[0] = 'B';
    hdr[1] = 'M';
    wr32(hdr + 2, file_size);
    wr32(hdr + 10, 54);    // pixel data offset
    wr32(hdr + 14, 40);    // info header size
    wr32(hdr + 18, (uint32_t)w);
    wr32(hdr + 22, (uint32_t)h);
    hdr[26] = 1;           // planes
    hdr[28] = 24;          // bpp
    wr32(hdr + 34, img_size);
    wr32(hdr + 38, 2835);  // 72 dpi
    wr32(hdr + 42, 2835);

    char name[32];
    snprintf(name, sizeof(name), "/photos/IMG_%04d.bmp", s_next_idx);

    File f = SD_MMC.open(name, FILE_WRITE, true);
    if (!f) {
        set_status("SD open failed");
        ESP_LOGE(TAG, "open %s failed", name);
        return;
    }
    bool ok = f.write(hdr, sizeof(hdr)) == sizeof(hdr) && f.write(s_bgr, img_size) == img_size;
    f.close();

    if (ok) {
        set_status("saved IMG_%04d.bmp", s_next_idx);
        ESP_LOGI(TAG, "saved %s (%u bytes)", name, (unsigned)file_size);
        s_next_idx++;
    } else {
        set_status("SD write failed");
        ESP_LOGE(TAG, "write %s failed", name);
    }
}

static void writer_task(void*) {
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (!save_jpeg()) {
            save_bmp();
        }
        s_busy = false;
    }
}

bool photo_store_init() {
    s_lock = xSemaphoreCreateMutex();

    if (!SD_MMC.begin("/sdcard")) {
        set_status("no SD card");
        ESP_LOGE(TAG, "SD_MMC mount failed - card inserted?");
        return false;
    }
    s_mounted = true;

    if (!SD_MMC.exists("/photos")) {
        SD_MMC.mkdir("/photos");
    }

    // Continue numbering after whatever is already on the card (.jpg or .bmp)
    File dir = SD_MMC.open("/photos");
    if (dir) {
        File e;
        while ((e = dir.openNextFile())) {
            int idx = 0;
            if (sscanf(e.name(), "IMG_%d", &idx) == 1 && idx >= s_next_idx) {
                s_next_idx = idx + 1;
            }
            e.close();
        }
        dir.close();
    }

    // Hardware JPEG encode engine; on failure photos fall back to BMP
    jpeg_encode_engine_cfg_t eng_cfg = {};
    eng_cfg.timeout_ms = 1000;
    if (jpeg_new_encoder_engine(&eng_cfg, &s_jpeg) != ESP_OK) {
        s_jpeg = NULL;
        ESP_LOGW(TAG, "hw jpeg engine unavailable, photos will be BMP");
    }

    xTaskCreate(writer_task, "photo_wr", 6144, NULL, 2, &s_writer);

    set_status("SD ready (%llu MB)", SD_MMC.cardSize() / (1024ULL * 1024ULL));
    ESP_LOGI(TAG, "SD mounted, %llu MB, next photo IMG_%04d.bmp",
             SD_MMC.cardSize() / (1024ULL * 1024ULL), s_next_idx);
    return true;
}

bool photo_store_capture(const uint8_t* rgb565, int w, int h) {
    if (!s_mounted || s_busy || rgb565 == NULL) return false;

    if (s_frame == NULL) {
        if (s_jpeg != NULL) {
            // JPEG DMA reads this buffer: let the driver pick the alignment
            jpeg_encode_memory_alloc_cfg_t mc = {.buffer_direction = JPEG_ENC_ALLOC_INPUT_BUFFER};
            size_t got = 0;
            s_frame = (uint8_t*)jpeg_alloc_encoder_mem((size_t)w * h * 2, &mc, &got);
        } else {
            s_frame = (uint8_t*)heap_caps_aligned_alloc(64, (size_t)w * h * 2, MALLOC_CAP_SPIRAM);
        }
    }
    if (s_frame == NULL) {
        set_status("out of memory");
        return false;
    }

    s_w = w;
    s_h = h;
    memcpy(s_frame, rgb565, (size_t)w * h * 2);
    s_busy = true;
    set_status("saving...");
    xTaskNotifyGive(s_writer);
    return true;
}
