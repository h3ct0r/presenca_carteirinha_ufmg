#include "rfid/pn532_reader.h"

#include <Adafruit_PN532.h>
#include <Wire.h>
#include <string.h>

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

static void rfid_task(void*) {
    // Debounce: while the same card sits on the reader, report it only once.
    uint8_t last_uid[7] = {0};
    uint8_t last_len = 0;

    for (;;) {
        uint8_t uid[7] = {0};
        uint8_t uid_len = 0;

        // Blocks only this task, up to 500 ms per attempt. Each internal
        // readiness poll is a short I2C read, so touch/camera transactions
        // interleave freely on the bus.
        if (nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uid_len, 500)) {
            bool same = (uid_len == last_len) && (memcmp(uid, last_uid, uid_len) == 0);
            if (!same && uid_len > 0) {
                memcpy(last_uid, uid, uid_len);
                last_len = uid_len;
                ESP_LOGI(TAG, "card detected (uid %u bytes)", uid_len);
                if (s_card_cb) s_card_cb(uid, uid_len);
            }
        } else {
            // Field is empty again: allow the same card to re-trigger.
            last_len = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

bool pn532_reader_start(pn532_card_cb_t on_card) {
    s_card_cb = on_card;

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
