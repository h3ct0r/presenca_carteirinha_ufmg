#include "camera/ov02c10_camera.h"

#include "camera/ov02c10_gain_map.h"
#include "camera/ov02c10_init_regs.h"
#include "driver/i2c_master.h"
#include "esp32-hal-log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define OV02C10_ADDR 0x36
#define OV02C10_CHIP_ID 0x5602
#define OV02C10_REG_CHIP_ID_H 0x300A
#define OV02C10_REG_CHIP_ID_L 0x300B
#define OV02C10_REG_STREAM 0x0100
#define OV02C10_REG_SW_RESET 0x0103

static const char* TAG = "ov02c10";

ov02c10_camera::ov02c10_camera(uint8_t i2c_port) : _port(i2c_port), _dev(nullptr) {}

bool ov02c10_camera::write_reg(uint16_t reg, uint8_t val) {
    uint8_t buf[3] = {(uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF), val};
    return i2c_master_transmit((i2c_master_dev_handle_t)_dev, buf, 3, 100) == ESP_OK;
}

int ov02c10_camera::read_reg(uint16_t reg) {
    uint8_t addr[2] = {(uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF)};
    uint8_t val = 0;
    if (i2c_master_transmit_receive((i2c_master_dev_handle_t)_dev, addr, 2, &val, 1, 100) != ESP_OK) {
        return -1;
    }
    return val;
}

bool ov02c10_camera::begin() {
    i2c_master_bus_handle_t bus = NULL;
    if (i2c_master_get_bus_handle(_port, &bus) != ESP_OK) {
        ESP_LOGE(TAG, "i2c bus %d not initialized", _port);
        return false;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = OV02C10_ADDR,
        .scl_speed_hz = 100000,
    };
    i2c_master_dev_handle_t dev = NULL;
    if (i2c_master_bus_add_device(bus, &dev_cfg, &dev) != ESP_OK) {
        ESP_LOGE(TAG, "failed to add I2C device 0x%02X", OV02C10_ADDR);
        return false;
    }
    _dev = dev;

    int id_h = read_reg(OV02C10_REG_CHIP_ID_H);
    int id_l = read_reg(OV02C10_REG_CHIP_ID_L);
    if (id_h < 0 || id_l < 0) {
        ESP_LOGE(TAG, "no I2C ACK from sensor - is the camera module connected?");
        return false;
    }
    uint16_t chip_id = ((uint16_t)id_h << 8) | (uint16_t)id_l;
    if (chip_id != OV02C10_CHIP_ID) {
        ESP_LOGE(TAG, "unexpected chip ID 0x%04X (expected 0x5602)", chip_id);
        return false;
    }
    ESP_LOGI(TAG, "OV02C10 found, chip ID 0x%04X", chip_id);

    for (size_t i = 0; i < sizeof(ov02c10_init_2lane_1080p30) / sizeof(ov02c10_reginfo_t); i++) {
        const ov02c10_reginfo_t* r = &ov02c10_init_2lane_1080p30[i];
        if (r->reg == OV02C10_REG_DELAY) {
            vTaskDelay(pdMS_TO_TICKS(r->val));
            continue;
        }
        if (!write_reg(r->reg, r->val)) {
            ESP_LOGE(TAG, "init failed at reg 0x%04X (index %d)", r->reg, (int)i);
            return false;
        }
        if (r->reg == OV02C10_REG_SW_RESET) {
            vTaskDelay(pdMS_TO_TICKS(10));  // settle after software reset
        }
    }
    ESP_LOGI(TAG, "init table written (2-lane RAW10 1920x1080@30)");
    return true;
}

bool ov02c10_camera::stream(bool on) {
    bool ok = write_reg(OV02C10_REG_STREAM, on ? 0x01 : 0x00);
    ESP_LOGI(TAG, "stream %s%s", on ? "ON" : "OFF", ok ? "" : " (FAILED)");
    return ok;
}

bool ov02c10_camera::set_exposure(uint16_t lines) {
    if (lines < EXPOSURE_MIN) lines = EXPOSURE_MIN;
    if (lines > EXPOSURE_MAX) lines = EXPOSURE_MAX;
    return write_reg(0x3501, lines >> 8) && write_reg(0x3502, lines & 0xFF);
}

bool ov02c10_camera::set_gain_index(int idx) {
    if (idx < 0) idx = 0;
    if (idx >= OV02C10_GAIN_STEPS) idx = OV02C10_GAIN_STEPS - 1;
    const ov02c10_gain_t* g = &ov02c10_gain_map[idx];
    return write_reg(0x3509, g->dgain_fine) && write_reg(0x3508, g->dgain_coarse) &&
           write_reg(0x350A, g->analog);
}
