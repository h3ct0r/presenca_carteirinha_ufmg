#include "services/battery_service.h"

#include <Arduino.h>

#include "app/battery_curve.h"
#include "app/event_bus.h"
#include "esp32-hal-log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char* TAG = "batt";

// GPIO53 = ADC2_CHANNEL4 on the ESP32-P4 (datasheet Table 2-7). ADC2 is
// freely usable here: the P4 has no native radio, so the classic ADC2-vs-WiFi
// contention doesn't apply (WiFi lives on the external ESP32-C6).
static constexpr int BAT_ADC_PIN = 53;

// Reverse of the resistor divider: V_BAT = V_node * (R52 + R57) / R57.
// R52/R57/R55 are on schematic sheet 4 (docs/hardware/schematic/4_USB&IO.png):
// BAT+ -> 68K -> node -> 100K -> GND, node -> 0R -> GPIO53, with C61 (10 nF)
// filtering the node. The tap is BAT+ (the cell), not the boosted VOUT-BAT.
static constexpr float DIVIDER_GAIN = (68.0f + 100.0f) / 100.0f;  // 1.68

// --- per-device calibration --------------------------------------------------
//
// The reading comes out low against a multimeter, for three reasons that are all
// systematic and none of which firmware can remove at the source:
//
//  - The divider is high-impedance: R52||R57 is ~40 kOhm, well above the
//    <10 kOhm the SAR ADC wants, so its sampling capacitor does not fully charge
//    within the sample window and the node reads low. C61 (10 nF) helps but is
//    small for this; averaging does NOT fix it, since the error is a bias, not
//    noise.
//  - 11 dB attenuation is linear to roughly 2450 mV, and a full 4.2 V cell puts
//    the node at ~2500 mV — right at the top, where the transfer curve
//    compresses. The error therefore grows with charge.
//  - R52/R57 tolerance moves the real ratio off the nominal 1.68, and the eFuse
//    ADC calibration has its own spread.
//
// A straight line fitted on the device removes all three at once:
//     corrected = reported * SCALE + OFFSET_MV
//
// To fit it, build the firmware with BATTERY_DRAIN_LOG (see platformio.ini),
// read the "node ... -> vbat ..." lines below while measuring BAT+ with a
// multimeter, and take two points as far apart as practical (say ~4.1 V and
// ~3.4 V — a fit over a narrow span amplifies measurement error):
//     SCALE  = (true_hi - true_lo) / (reported_hi - reported_lo)
//     OFFSET = true_lo - SCALE * reported_lo
//
// Fitted 2026-07-31 on this unit: a multimeter read 3860 mV on BAT+ while the
// log reported 3670 mV — 4.9% low, which a 5% pair of divider resistors would
// produce on its own. Scale-only: one point cannot separate a gain error from an
// offset, and every dominant cause here (resistor ratio, incomplete settling,
// ADC gain) is proportional, so a slope degrades far more gracefully across the
// range than a constant would. A second point near 3.4 V would confirm it.
//
// PER-BOARD. Another unit has its own resistors and its own eFuse calibration;
// this number does not transfer.
static constexpr float BAT_CAL_SCALE = 1.0518f;  // 3860 / 3670
static constexpr int BAT_CAL_OFFSET_MV = 0;

static constexpr uint32_t SAMPLE_PERIOD_MS = 10000;
static constexpr int OVERSAMPLE = 16;  // average out ADC noise (not the bias above)

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
        float raw_mv = node_mv * DIVIDER_GAIN;
        int32_t vbat_mv = (int32_t)(raw_mv * BAT_CAL_SCALE + 0.5f) + BAT_CAL_OFFSET_MV;
        if (vbat_mv < 0) vbat_mv = 0;  // a negative offset must not wrap the uint16

#ifdef BATTERY_DRAIN_LOG
        // The calibration dataset: pair these with a multimeter on BAT+.
        ESP_LOGI(TAG, "node %u mV -> vbat %d mV (uncalibrated %d)", (unsigned)node_mv,
                 (int)vbat_mv, (int)(raw_mv + 0.5f));
#endif

        app_event_t ev = {};
        ev.type = APP_EVENT_POWER_STATE;
        ev.power.pct = battery_mv_to_pct((uint16_t)vbat_mv);
        ev.power.mv = (uint16_t)vbat_mv;
        event_bus_post(&ev);

        vTaskDelay(pdMS_TO_TICKS(SAMPLE_PERIOD_MS));
    }
}

bool battery_service_start(void) {
    return xTaskCreate(battery_task, "battery", 3072, nullptr, 2, nullptr) == pdPASS;
}
