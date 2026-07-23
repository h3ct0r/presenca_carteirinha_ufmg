#include "audio/beeper.h"

#include <math.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp32-hal-log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
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
#define RAMP_MS 8  // fade in/out to avoid clicks

// The kinds of beep, each a prebuilt tone buffer. Selected via the queue.
typedef enum { BEEP_CONFIRM,
               BEEP_TOUCH,
               BEEP_ERROR,
               BEEP_COUNT } beep_type_t;

// One tone segment: a sine at hz for ms, at amplitude amp (amp 0 = silence).
typedef struct {
    int hz;
    int ms;
    float amp;
} beep_burst_t;

// Confirmation: a single clear mid beep. Touch: short, quiet, higher tick.
// Error: a descending two-tone "denied" — frequencies kept in the buzzer's
// reproducible range (very low tones like 340 Hz are inaudible on it), the
// downward pitch + double pattern making it clearly different from confirm.
// static const beep_burst_t TOUCH_BURSTS[] = {{2700, 22, 0.08f}};
//                                       freq─┘  ms─┘  amp─┘  (0.0–1.0)
static const beep_burst_t CONFIRM_BURSTS[] = {{2000, 50, 0.10f}};
static const beep_burst_t TOUCH_BURSTS[] = {{2700, 22, 0.16f}};
static const beep_burst_t ERROR_BURSTS[] = {{1600, 90, 0.35f}, {0, 45, 0.0f}, {1100, 150, 0.35f}};

typedef struct {
    int16_t* buf;  // stereo interleaved samples
    size_t bytes;
} beep_tone_t;

static i2s_chan_handle_t s_tx = NULL;
static i2c_master_dev_handle_t s_codec = NULL;
static TaskHandle_t s_task = NULL;
static QueueHandle_t s_queue = NULL;
static beep_tone_t s_tones[BEEP_COUNT];

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

// Builds one tone by concatenating its bursts, each with a short fade in/out
// to avoid clicks. Leaves tone->buf NULL on allocation failure (that beep
// then becomes a no-op).
static void build_tone(beep_tone_t* tone, const beep_burst_t* bursts, int n) {
    int total = 0;
    for (int b = 0; b < n; b++) total += SAMPLE_RATE * bursts[b].ms / 1000;

    tone->bytes = (size_t)total * 2 * sizeof(int16_t);
    tone->buf = (int16_t*)heap_caps_malloc(tone->bytes, MALLOC_CAP_DEFAULT);
    if (tone->buf == NULL) {
        tone->bytes = 0;
        return;
    }

    int idx = 0;
    for (int b = 0; b < n; b++) {
        int frames = SAMPLE_RATE * bursts[b].ms / 1000;
        int ramp = SAMPLE_RATE * RAMP_MS / 1000;
        if (ramp * 2 > frames) ramp = frames / 2;
        for (int i = 0; i < frames; i++) {
            float env = 1.0f;
            if (ramp > 0 && i < ramp) env = (float)i / ramp;
            if (ramp > 0 && frames - 1 - i < ramp) env = (float)(frames - 1 - i) / ramp;
            int16_t v = (int16_t)(bursts[b].amp * env * 32767.0f *
                                  sinf(2.0f * (float)M_PI * bursts[b].hz * i / SAMPLE_RATE));
            tone->buf[idx * 2 + 0] = v;
            tone->buf[idx * 2 + 1] = v;
            idx++;
        }
    }
}

static void build_all_tones() {
    build_tone(&s_tones[BEEP_CONFIRM], CONFIRM_BURSTS,
               sizeof(CONFIRM_BURSTS) / sizeof(CONFIRM_BURSTS[0]));
    build_tone(&s_tones[BEEP_TOUCH], TOUCH_BURSTS,
               sizeof(TOUCH_BURSTS) / sizeof(TOUCH_BURSTS[0]));
    build_tone(&s_tones[BEEP_ERROR], ERROR_BURSTS,
               sizeof(ERROR_BURSTS) / sizeof(ERROR_BURSTS[0]));
}

static void beeper_task(void*) {
    beep_type_t type;
    for (;;) {
        if (xQueueReceive(s_queue, &type, portMAX_DELAY) != pdTRUE) continue;
        if (type >= BEEP_COUNT || s_tones[type].buf == NULL) continue;
        size_t written = 0;
        i2s_channel_write(s_tx, s_tones[type].buf, s_tones[type].bytes, &written,
                          pdMS_TO_TICKS(1000));
    }
}

static void queue_beep(beep_type_t type) {
    if (s_queue != NULL) xQueueSend(s_queue, &type, 0);
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

    build_all_tones();
    if (s_tones[BEEP_TOUCH].buf == NULL && s_tones[BEEP_CONFIRM].buf == NULL) return false;

    // Enable the NS4150 last to avoid the power-up pop
    gpio_config_t pa = {};
    pa.pin_bit_mask = 1ULL << PA_CTRL;
    pa.mode = GPIO_MODE_OUTPUT;
    gpio_config(&pa);
    gpio_set_level(PA_CTRL, 1);

    s_queue = xQueueCreate(4, sizeof(beep_type_t));
    if (s_queue == NULL) return false;
    xTaskCreate(beeper_task, "beeper", 4096, NULL, 3, &s_task);
    ESP_LOGI(TAG, "audio path up (ES8311 + NS4150 on CN1)");
    return true;
}

void beeper_beep() { queue_beep(BEEP_CONFIRM); }
void beeper_touch() { queue_beep(BEEP_TOUCH); }
void beeper_error() { queue_beep(BEEP_ERROR); }
