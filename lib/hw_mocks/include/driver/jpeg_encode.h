#pragma once

// Mock of the ESP32-P4 hardware JPEG encoder driver (only what photo_store
// uses). The "encoder" writes a recognizable fake JPEG so tests can assert
// on the written file. Control: mock_jpeg.h.

#include <stddef.h>
#include <stdint.h>

#ifndef ESP_OK
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#endif

typedef struct mock_jpeg_encoder* jpeg_encoder_handle_t;

typedef struct {
    int timeout_ms;
} jpeg_encode_engine_cfg_t;

typedef enum { JPEG_ENCODE_IN_FORMAT_RGB565 } jpeg_enc_input_format_t;
typedef enum { JPEG_DOWN_SAMPLING_YUV420 } jpeg_down_sampling_type_t;

typedef struct {
    uint32_t width;
    uint32_t height;
    jpeg_enc_input_format_t src_type;
    jpeg_down_sampling_type_t sub_sample;
    uint32_t image_quality;
} jpeg_encode_cfg_t;

typedef enum {
    JPEG_ENC_ALLOC_INPUT_BUFFER,
    JPEG_ENC_ALLOC_OUTPUT_BUFFER,
} jpeg_enc_buffer_direction_t;

typedef struct {
    jpeg_enc_buffer_direction_t buffer_direction;
} jpeg_encode_memory_alloc_cfg_t;

esp_err_t jpeg_new_encoder_engine(const jpeg_encode_engine_cfg_t* cfg,
                                  jpeg_encoder_handle_t* out);
void* jpeg_alloc_encoder_mem(size_t size, const jpeg_encode_memory_alloc_cfg_t* cfg,
                             size_t* actual_size);
esp_err_t jpeg_encoder_process(jpeg_encoder_handle_t handle, const jpeg_encode_cfg_t* cfg,
                               const uint8_t* src, uint32_t src_len, uint8_t* dst,
                               uint32_t dst_cap, uint32_t* out_len);
