#ifndef _OV02C10_CAMERA_H
#define _OV02C10_CAMERA_H
#include <stdint.h>

// OV02C10 2MP MIPI-CSI sensor on the Guition camera FPC.
// The module self-clocks and has no reset/pwdn lines on this board:
// bring-up is pure I2C (addr 0x36, 16-bit registers).
class ov02c10_camera {
public:
    ov02c10_camera(uint8_t i2c_port = 1);

    bool begin();  // probe chip ID, write full 2-lane 1080p30 init (streaming off)
    bool stream(bool on);

    // Manual exposure control (no hardware AE on this pipeline)
    bool set_exposure(uint16_t lines);   // clamped to [8, 1149] (VTS - 15)
    bool set_gain_index(int idx);        // index into the vendor gain ladder (0..191, 1x..63x)

    static constexpr uint16_t EXPOSURE_MIN = 8;
    static constexpr uint16_t EXPOSURE_MAX = 1149;

    static constexpr uint16_t WIDTH = 1920;
    static constexpr uint16_t HEIGHT = 1080;

private:
    bool write_reg(uint16_t reg, uint8_t val);
    int read_reg(uint16_t reg);

    uint8_t _port;
    void* _dev;  // i2c_master_dev_handle_t
};

#endif
