#include "services/battery_service.h"

#include <Arduino.h>

#include "app/battery_curve.h"
#include "app/event_bus.h"
#include "esp32-hal-log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char* TAG = "battery";

// GPIO53 = ADC2_CHANNEL4 on the ESP32-P4 (datasheet Table 2-7). ADC2 is
// freely usable here: the P4 has no native radio, so the classic ADC2-vs-WiFi
// contention doesn't apply (WiFi lives on the external ESP32-C6).
static constexpr int BAT_ADC_PIN = 53;

// Reverse of the resistor divider: V_BAT = V_node * (R52 + R57) / R57.
static constexpr float DIVIDER_GAIN = (68.0f + 100.0f) / 100.0f;  // 1.68

static constexpr uint32_t SAMPLE_PERIOD_MS = 10000;
static constexpr int OVERSAMPLE = 16;  // average out ADC noise

static void battery_task(void*) {
    analogReadResolution(12);
    // 12 dB attenuation covers the ~2.5 V the node reaches at a full 4.2 V cell.
    analogSetPinAttenuation(BAT_ADC_PIN, ADC_11db);

    for (;;) {
        uint32_t acc = 0;
        for (int i = 0; i < OVERSAMPLE; i++) {
            acc += analogReadMilliVolts(BAT_ADC_PIN);  // eFuse-calibrated mV
        }
        uint32_t node_mv = acc / OVERSAMPLE;
        uint32_t vbat_mv = (uint32_t)(node_mv * DIVIDER_GAIN + 0.5f);

        app_event_t ev = {};
        ev.type = APP_EVENT_POWER_STATE;
        ev.power.pct = battery_mv_to_pct((uint16_t)vbat_mv);
        ev.power.mv = (uint16_t)vbat_mv;
        event_bus_post(&ev);

        // ESP_LOGI(TAG, "node=%umV vbat=%umV pct=%u", (unsigned)node_mv, (unsigned)vbat_mv,
        //          ev.power.pct);
        vTaskDelay(pdMS_TO_TICKS(SAMPLE_PERIOD_MS));
    }
}

bool battery_service_start(void) {
    return xTaskCreate(battery_task, "battery", 3072, nullptr, 2, nullptr) == pdPASS;
}
