#include "rfid/pn532_reader.h"

#include <Adafruit_PN532.h>
#include <Wire.h>
#include <string.h>

#include "app/card_gate.h"

#include "esp32-hal-log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char* TAG = "pn532";

// The PN532 hangs off connector CN3, which carries the shared ES_I2C bus
// (GPIO7 SDA / GPIO8 SCL) also used by the GT911 touch, ES8311 codec and
// camera sensor. Those peripherals talk through the IDF i2c_master driver;
// the Adafruit library talks through Wire1 (same port, see main.cpp). The
// IDF driver serializes transactions per bus, so they coexist safely.
//
// No IRQ/RSTPD_N lines on CN3: the library polls the PN532's I2C RDY status
// byte instead, so -1/-1 is fine here.
static Adafruit_PN532 nfc(-1, -1, &Wire1);

static pn532_card_cb_t s_card_cb = nullptr;
static pn532_collision_cb_t s_collision_cb = nullptr;

static void rfid_task(void*) {
    card_gate_reset();

    for (;;) {
        uint8_t uid[10] = {0};
        uint8_t uid_len = 0;

        // Blocks only this task, up to 500 ms per attempt. Each internal
        // readiness poll is a short I2C read, so touch/camera transactions
        // interleave freely on the bus.
        //
        // This asks for ONE target (MaxTg=1), so with several cards on the
        // reader it returns whichever wins anticollision that time round —
        // which alternates. card_gate turns the raw stream into decisions;
        // see app/card_gate.h for why it cannot be a simple debounce.
        bool present = nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uid_len, 500);
        if (!present) uid_len = 0;

        uint8_t accepted[10] = {0};
        uint8_t accepted_len = 0;
        switch (card_gate_poll(uid, uid_len, accepted, &accepted_len)) {
            case CARD_GATE_ACCEPT: {
                char buf[3 * 10 + 1] = {0};
                for (uint8_t i = 0; i < accepted_len && i < 10; i++) {
                    snprintf(buf + i * 3, sizeof(buf) - i * 3, "%02X ", accepted[i]);
                }
                ESP_LOGI(TAG, "RFID card UID: %s", buf);
                // The beep is decided by the auth outcome (grant vs. deny) in
                // the UI, not on raw detection — a rejected card must not get
                // the confirmation beep.
                if (s_card_cb) s_card_cb(accepted, accepted_len);
                break;
            }
            case CARD_GATE_COLLISION:
                ESP_LOGW(TAG, "several cards on the reader — ignoring until it is cleared");
                if (s_collision_cb) s_collision_cb();
                break;
            case CARD_GATE_IGNORE:
                break;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

bool pn532_reader_start(pn532_card_cb_t on_card, pn532_collision_cb_t on_collision) {
    s_card_cb = on_card;
    s_collision_cb = on_collision;

    nfc.begin();

    uint32_t ver = nfc.getFirmwareVersion();
    if (!ver) {
        ESP_LOGE(TAG, "PN532 not found on CN3 (check cable / I2C mode switches)");
        return false;
    }
    ESP_LOGI(TAG, "PN532 firmware %lu.%lu", (unsigned long)((ver >> 16) & 0xFF),
             (unsigned long)((ver >> 8) & 0xFF));

    // Normal (SAM) mode; required before readPassiveTargetID sees anything.
    if (!nfc.SAMConfig()) {
        ESP_LOGE(TAG, "SAMConfig failed");
        return false;
    }

    // Priority 3: below the camera/detection pipeline, above idle.
    xTaskCreate(rfid_task, "rfid", 4096, nullptr, 3, nullptr);
    return true;
}
