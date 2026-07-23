#include "mock_jpeg.h"

#include <stdlib.h>
#include <string.h>

#include "driver/jpeg_encode.h"

static bool g_engine_available = true;

void mock_jpeg_set_engine_available(bool available) { g_engine_available = available; }

esp_err_t jpeg_new_encoder_engine(const jpeg_encode_engine_cfg_t*,
                                  jpeg_encoder_handle_t* out) {
    if (!g_engine_available) {
        *out = nullptr;
        return ESP_FAIL;
    }
    *out = (jpeg_encoder_handle_t)malloc(1);
    return ESP_OK;
}

void* jpeg_alloc_encoder_mem(size_t size, const jpeg_encode_memory_alloc_cfg_t*,
                             size_t* actual_size) {
    if (actual_size) *actual_size = size;
    return malloc(size);
}

esp_err_t jpeg_encoder_process(jpeg_encoder_handle_t handle, const jpeg_encode_cfg_t*,
                               const uint8_t*, uint32_t, uint8_t* dst, uint32_t dst_cap,
                               uint32_t* out_len) {
    if (!handle || dst_cap < 16) return ESP_FAIL;
    // SOI marker + a recognizable payload for assertions.
    dst[0] = 0xFF;
    dst[1] = 0xD8;
    memcpy(dst + 2, "MOCKJPG", 7);
    *out_len = 64 < dst_cap ? 64 : dst_cap;
    return ESP_OK;
}
