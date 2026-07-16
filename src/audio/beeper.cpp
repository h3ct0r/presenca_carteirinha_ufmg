#include "audio/beeper.h"

#include <math.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp32-hal-log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char* TAG = "beeper";

// Audio wiring on this board (see docs/schematic 3_ESP32-P4 / 6_CODEC / 1_PWR):
// I2S0 to the ES8311 codec, whose DAC feeds the NS4150 amp behind CN1.
#define I2S_MCLK GPIO_NUM_13  // CODEC_I2S0_MCLK
#define I2S_BCLK GPIO_NUM_12  // CODEC_I2S0_SCLK
#define I2S_WS GPIO_NUM_10    // CODEC_I2S0_LRCK
#define I2S_DOUT GPIO_NUM_9   // CODEC_I2S0_DSDIN
// Amp enable. The module pin named PA_CTRL sits between GPIO10 and the C6
// UART pins, and GPIO11 is the only GPIO in that range used nowhere else.
#define PA_CTRL GPIO_NUM_11

#define ES8311_ADDR 0x18
#define SAMPLE_RATE 16000
#define BEEP_HZ 2000
#define BEEP_MS 50
#define RAMP_MS 8  // fade in/out to avoid clicks
#define AMPLITUDE 0.25f

static i2s_chan_handle_t s_tx = NULL;
static i2c_master_dev_handle_t s_codec = NULL;
static TaskHandle_t s_task = NULL;
static int16_t* s_tone = NULL;  // stereo interleaved beep samples
static size_t s_tone_bytes = 0;

static bool es8311_write(uint8_t reg, uint8_t val) {
    uint8_t buf[2] = {reg, val};
    return i2c_master_transmit(s_codec, buf, sizeof(buf), 100) == ESP_OK;
}

// Minimal ES8311 playback bring-up: I2S slave, 16-bit, MCLK = 256*fs from
// the MCLK pad (sequence follows Espressif's es8311 codec driver).
static bool es8311_init() {
    static const uint8_t regs[][2] = {
        {0x00, 0x80},  // power on, slave mode
        {0x01, 0x3F},  // MCLK from pad, all clocks on
        {0x02, 0x00},  // pre_div 1, pre_mult x1
        {0x03, 0x10},  // single speed, ADC OSR
        {0x04, 0x10},  // DAC OSR
        {0x05, 0x00},  // adc_div 1, dac_div 1
        {0x06, 0x03},  // BCLK = MCLK/4 (64*fs)
        {0x07, 0x00},  // LRCK = MCLK/256 (high byte)
        {0x08, 0xFF},  // LRCK divider (low byte)
        {0x09, 0x0C},  // DAC: I2S, 16-bit
        {0x0A, 0x0C},  // ADC: I2S, 16-bit
        {0x0B, 0x00},
        {0x0C, 0x00},
        {0x0D, 0x01},  // power up analog
        {0x0E, 0x02},
        {0x12, 0x00},  // power up DAC
        {0x13, 0x10},  // enable output stage
        {0x14, 0x1A},
        {0x1C, 0x6A},
        {0x37, 0x08},  // DAC ramp / EQ bypass
        {0x32, 0xB0},  // DAC volume (a bit under 0 dB; buzzer is close-range)
        {0x31, 0x00},  // unmute
    };

    // Reset first; a failed ACK here means no codec on the bus
    if (!es8311_write(0x00, 0x1F)) return false;
    vTaskDelay(pdMS_TO_TICKS(20));
    es8311_write(0x00, 0x00);

    for (size_t i = 0; i < sizeof(regs) / sizeof(regs[0]); i++) {
        if (!es8311_write(regs[i][0], regs[i][1])) return false;
    }
    return true;
}

static void make_tone() {
    const int frames = SAMPLE_RATE * BEEP_MS / 1000;
    const int ramp = SAMPLE_RATE * RAMP_MS / 1000;
    s_tone_bytes = (size_t)frames * 2 * sizeof(int16_t);
    s_tone = (int16_t*)heap_caps_malloc(s_tone_bytes, MALLOC_CAP_DEFAULT);
    if (s_tone == NULL) {
        s_tone_bytes = 0;
        return;
    }
    for (int i = 0; i < frames; i++) {
        float env = 1.0f;
        if (i < ramp) env = (float)i / ramp;
        if (frames - 1 - i < ramp) env = (float)(frames - 1 - i) / ramp;
        int16_t v = (int16_t)(AMPLITUDE * env * 32767.0f *
                              sinf(2.0f * (float)M_PI * BEEP_HZ * i / SAMPLE_RATE));
        s_tone[i * 2 + 0] = v;
        s_tone[i * 2 + 1] = v;
    }
}

static void beeper_task(void*) {
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        size_t written = 0;
        i2s_channel_write(s_tx, s_tone, s_tone_bytes, &written, pdMS_TO_TICKS(1000));
    }
}

bool beeper_init() {
    // Codec sits on the shared I2C bus (port 1) like touch/camera
    i2c_master_bus_handle_t bus = NULL;
    if (i2c_master_get_bus_handle(1, &bus) != ESP_OK) {
        ESP_LOGE(TAG, "I2C bus not initialized");
        return false;
    }
    i2c_device_config_t dev_cfg = {};
    dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_cfg.device_address = ES8311_ADDR;
    dev_cfg.scl_speed_hz = 100000;
    if (i2c_master_bus_add_device(bus, &dev_cfg, &s_codec) != ESP_OK) {
        return false;
    }

    // I2S master: MCLK 256*fs, 16-bit stereo. auto_clear keeps the line at
    // silence between beeps instead of looping stale DMA data.
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    if (i2s_new_channel(&chan_cfg, &s_tx, NULL) != ESP_OK) {
        ESP_LOGE(TAG, "i2s channel alloc failed");
        return false;
    }
    i2s_std_config_t std_cfg = {};
    std_cfg.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE);
    std_cfg.slot_cfg =
        I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
    std_cfg.gpio_cfg.mclk = I2S_MCLK;
    std_cfg.gpio_cfg.bclk = I2S_BCLK;
    std_cfg.gpio_cfg.ws = I2S_WS;
    std_cfg.gpio_cfg.dout = I2S_DOUT;
    std_cfg.gpio_cfg.din = I2S_GPIO_UNUSED;
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_tx, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(s_tx));  // MCLK must run before codec init

    if (!es8311_init()) {
        ESP_LOGE(TAG, "ES8311 codec not responding");
        return false;
    }

    make_tone();
    if (s_tone == NULL) return false;

    // Enable the NS4150 last to avoid the power-up pop
    gpio_config_t pa = {};
    pa.pin_bit_mask = 1ULL << PA_CTRL;
    pa.mode = GPIO_MODE_OUTPUT;
    gpio_config(&pa);
    gpio_set_level(PA_CTRL, 1);

    xTaskCreate(beeper_task, "beeper", 4096, NULL, 3, &s_task);
    ESP_LOGI(TAG, "audio path up (ES8311 + NS4150 on CN1)");
    return true;
}

void beeper_beep() {
    if (s_task != NULL) {
        xTaskNotifyGive(s_task);
    }
}
