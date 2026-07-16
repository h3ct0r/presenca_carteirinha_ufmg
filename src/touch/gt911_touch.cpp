#include "touch/gt911_touch.h"

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_err.h"
// esp32-hal-log.h remaps ESP_LOG* to Arduino's logger; the IDF-native ones are
// compiled out of this framework build (CONFIG_LOG_MAXIMUM_LEVEL=ERROR).
#include "esp32-hal-log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "touch/esp_lcd_touch_gt911.h"

#define LCD_HRES 480
#define LCD_VRES 800

// Shared I2C bus used for the touch controller (see main.cpp: I2C_NUM_1).
#define TOUCH_I2C_PORT 1

// GT911 straps to one of two addresses depending on the INT level at reset.
#define GT911_ADDR_A 0x5D
#define GT911_ADDR_B 0x14

static const char* TAG = "gt911";

static esp_lcd_touch_handle_t s_tp = NULL;
static esp_lcd_panel_io_handle_t s_tp_io_handle = NULL;

// Scratch outputs for esp_lcd_touch_get_coordinates (single-point reads).
static uint16_t s_touch_strength[1];
static uint8_t s_touch_cnt = 0;

gt911_touch::gt911_touch(int8_t sda_pin, int8_t scl_pin, int8_t rst_pin, int8_t int_pin)
    : _sda(sda_pin), _scl(scl_pin), _rst(rst_pin), _int(int_pin) {}

void gt911_touch::begin() {
    i2c_master_bus_handle_t i2c_handle = NULL;
    i2c_master_get_bus_handle(TOUCH_I2C_PORT, &i2c_handle);

    // Release GT911 from reset before touching the bus; it needs ~50ms to boot.
    if (_rst >= 0) {
        gpio_config_t rst_conf = {};
        rst_conf.pin_bit_mask = 1ULL << _rst;
        rst_conf.mode = GPIO_MODE_OUTPUT;
        gpio_config(&rst_conf);
        gpio_set_level((gpio_num_t)_rst, 0);
        vTaskDelay(pdMS_TO_TICKS(10));
        gpio_set_level((gpio_num_t)_rst, 1);
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    esp_lcd_panel_io_i2c_config_t tp_io_config = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
    tp_io_config.scl_speed_hz = 100000;

    // Probe both possible strap addresses.
    if (i2c_master_probe(i2c_handle, GT911_ADDR_A, 100) == ESP_OK) {
        tp_io_config.dev_addr = GT911_ADDR_A;
    } else if (i2c_master_probe(i2c_handle, GT911_ADDR_B, 100) == ESP_OK) {
        tp_io_config.dev_addr = GT911_ADDR_B;
    } else {
        ESP_LOGE(TAG, "no GT911 ACK at 0x%02X or 0x%02X - check touch wiring/reset",
                 GT911_ADDR_A, GT911_ADDR_B);
    }
    ESP_LOGI(TAG, "Initialize touch IO (I2C), GT911 at 0x%02X", (unsigned)tp_io_config.dev_addr);
    esp_lcd_new_panel_io_i2c(i2c_handle, &tp_io_config, &s_tp_io_handle);

    esp_lcd_touch_config_t tp_cfg = {
        .x_max = LCD_HRES,
        .y_max = LCD_VRES,
        .rst_gpio_num = (gpio_num_t)_rst,
        .int_gpio_num = (gpio_num_t)_int,
        .levels = {
            .reset = 0,
            .interrupt = 0,
        },
        .flags = {
            .swap_xy = 0,
            .mirror_x = 0,
            .mirror_y = 0,
        },
    };

    ESP_LOGI(TAG, "Initialize touch controller gt911");
    ESP_ERROR_CHECK(esp_lcd_touch_new_i2c_gt911(s_tp_io_handle, &tp_cfg, &s_tp));
}

bool gt911_touch::getTouch(uint16_t* x, uint16_t* y) {
    esp_lcd_touch_read_data(s_tp);
    return esp_lcd_touch_get_coordinates(s_tp, x, y, s_touch_strength, &s_touch_cnt, 1);
}

void gt911_touch::set_rotation(uint8_t r) {
    // Only 0/180 and 90/270 pairs are distinguished (portrait vs. flipped).
    switch (r) {
        case 0:
        case 2:
            esp_lcd_touch_set_swap_xy(s_tp, false);
            esp_lcd_touch_set_mirror_x(s_tp, false);
            esp_lcd_touch_set_mirror_y(s_tp, false);
            break;
        case 1:
        case 3:
            esp_lcd_touch_set_swap_xy(s_tp, false);
            esp_lcd_touch_set_mirror_x(s_tp, true);
            esp_lcd_touch_set_mirror_y(s_tp, true);
            break;
    }
}
