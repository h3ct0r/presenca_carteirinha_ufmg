#include "camera/auto_exposure.h"

#include <math.h>
#include <stdlib.h>

#include "camera/csi_pipeline.h"
#include "camera/ov02c10_camera.h"
#include "camera/ov02c10_gain_map.h"

// Recompute statistics and drive the sensor/ISP once every N preview frames.
// Register writes land a frame or two later, so there is no value in running
// the loop on every single frame.
static constexpr uint32_t UPDATE_EVERY_N_FRAMES = 4;

// Target mean luma and the deadband around it within which exposure is left
// alone (avoids hunting).
static constexpr int LUMA_TARGET = 120;
static constexpr int LUMA_DEADBAND = 16;

// Mean luma (0..255) and per-channel means of an RGB565LE frame, subsampled for
// speed (every 16th pixel). The channel means feed the gray-world AWB loop.
static int preview_stats(const uint8_t* buf, int width, int height, uint32_t* r_mean,
                         uint32_t* g_mean, uint32_t* b_mean) {
    const uint16_t* px = (const uint16_t*)buf;
    const int total = width * height;
    uint32_t sum = 0, rs = 0, gs = 0, bs = 0, n = 0;
    for (int i = 0; i < total; i += 16, n++) {
        uint16_t p = px[i];
        uint8_t r = (p >> 8) & 0xF8, g = (p >> 3) & 0xFC, b = (p << 3) & 0xF8;
        rs += r;
        gs += g;
        bs += b;
        sum += (77 * r + 150 * g + 29 * b) >> 8;  // Rec.601 luma
    }
    if (n == 0) return 0;
    *r_mean = rs / n;
    *g_mean = gs / n;
    *b_mean = bs / n;
    return (int)(sum / n);
}

AutoExposure::AutoExposure(ov02c10_camera& sensor) : _sensor(sensor) {}

int AutoExposure::update(const uint8_t* preview, int width, int height) {
    if ((_frame++ % UPDATE_EVERY_N_FRAMES) != 0) {
        return _last_luma;
    }
    uint32_t rm = 0, gm = 0, bm = 0;
    _last_luma = preview_stats(preview, width, height, &rm, &gm, &bm);
    apply_exposure(_last_luma);
    apply_white_balance(rm, gm, bm);
    return _last_luma;
}

// Minimal software AE: nudge sensor exposure/gain toward the target mean luma.
// Exposure is the fine control; the vendor gain ladder (1x..63x) extends range
// in the dark once exposure is maxed out.
void AutoExposure::apply_exposure(int mean_luma) {
    if (abs(mean_luma - LUMA_TARGET) <= LUMA_DEADBAND) return;

    float ratio = (float)LUMA_TARGET / (mean_luma < 5 ? 5 : mean_luma);
    if (ratio > 2.0f) ratio = 2.0f;
    if (ratio < 0.5f) ratio = 0.5f;
    _ev *= ratio;

    const float ev_min = ov02c10_camera::EXPOSURE_MIN;
    const float ev_max =
        (float)ov02c10_camera::EXPOSURE_MAX * ov02c10_total_gain_x1000[OV02C10_GAIN_STEPS - 1] / 1000.0f;
    if (_ev < ev_min) _ev = ev_min;
    if (_ev > ev_max) _ev = ev_max;

    // Below the exposure ceiling, use exposure alone; above it, hold exposure at
    // max and pick the lowest gain step that reaches the requested light level.
    uint16_t exposure;
    int gain_idx = 0;
    if (_ev <= ov02c10_camera::EXPOSURE_MAX) {
        exposure = (uint16_t)_ev;
    } else {
        exposure = ov02c10_camera::EXPOSURE_MAX;
        uint32_t need_x1000 = (uint32_t)(_ev * 1000.0f / ov02c10_camera::EXPOSURE_MAX);
        while (gain_idx < OV02C10_GAIN_STEPS - 1 &&
               ov02c10_total_gain_x1000[gain_idx + 1] <= need_x1000) {
            gain_idx++;
        }
    }
    _sensor.set_exposure(exposure);
    _sensor.set_gain_index(gain_idx);
}

// Closed-loop gray-world AWB: the preview is post-CCM, so nudge the R/B gains
// (damped square-root steps) until the channel means match green, then push the
// gains into the ISP color-correction matrix.
void AutoExposure::apply_white_balance(uint32_t r_mean, uint32_t g_mean, uint32_t b_mean) {
    if (g_mean < 8 || r_mean < 2 || b_mean < 2) return;  // too dark to judge color

    _r_gain *= sqrtf((float)g_mean / r_mean);
    _b_gain *= sqrtf((float)g_mean / b_mean);
    if (_r_gain < 0.8f) _r_gain = 0.8f;
    if (_r_gain > 2.5f) _r_gain = 2.5f;
    if (_b_gain < 0.8f) _b_gain = 0.8f;
    if (_b_gain > 2.5f) _b_gain = 2.5f;
    csi_pipeline_set_wb(_r_gain, _b_gain);
}
