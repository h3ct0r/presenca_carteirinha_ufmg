#ifndef _AUTO_EXPOSURE_H
#define _AUTO_EXPOSURE_H

#include <stdint.h>

class ov02c10_camera;

// Software auto-exposure (AE) and gray-world auto-white-balance (AWB).
//
// The OV02C10 pipeline on this board has no hardware AE, so the loop is closed
// in firmware: brightness and per-channel color statistics are derived from the
// downscaled RGB565 preview, and the sensor exposure/gain and ISP color matrix
// are nudged toward a neutral, well-exposed image. Corrections are damped and
// only applied every few frames, since sensor register writes take effect a
// frame or two later.
//
// All state is held per-instance; create one and feed it every preview frame.
class AutoExposure {
public:
    explicit AutoExposure(ov02c10_camera& sensor);

    // Feed one preview frame (RGB565, width*height pixels). Every few calls it
    // recomputes statistics and updates the sensor and ISP. Returns the most
    // recent mean luma (0..255), handy for on-screen status.
    int update(const uint8_t* preview, int width, int height);

private:
    void apply_exposure(int mean_luma);
    void apply_white_balance(uint32_t r_mean, uint32_t g_mean, uint32_t b_mean);

    ov02c10_camera& _sensor;
    uint32_t _frame = 0;    // frames seen, for the update cadence
    int _last_luma = 0;     // most recent mean luma (0..255)
    float _ev = 800.0f;     // exposure-line equivalents (exposure * gain)
    float _r_gain = 1.0f;   // ISP red gain relative to green
    float _b_gain = 1.0f;   // ISP blue gain relative to green
};

#endif  // _AUTO_EXPOSURE_H
