#include "services/device_secret.h"

#include <string.h>

#include "esp32-hal-log.h"

static const char* TAG = "devsecret";

#ifdef NATIVE_TEST

// Host tests get a fixed key so fingerprints are reproducible across runs and
// can be asserted directly. Nothing else about the code path changes.
bool device_secret_get(uint8_t out[DEVICE_SECRET_LEN]) {
    for (size_t i = 0; i < DEVICE_SECRET_LEN; i++) out[i] = (uint8_t)(0xA0 + i);
    return true;
}

#else

#include <Preferences.h>
#include <esp_random.h>

static constexpr const char* NVS_NAMESPACE = "carteirinha";
static constexpr const char* NVS_KEY = "credkey";

// Cached after the first read: this is on the path of every card tap and every
// unlock, and opening NVS each time would be pointless I/O.
static uint8_t s_key[DEVICE_SECRET_LEN];
static bool s_have_key = false;

bool device_secret_get(uint8_t out[DEVICE_SECRET_LEN]) {
    if (s_have_key) {
        memcpy(out, s_key, DEVICE_SECRET_LEN);
        return true;
    }
    memset(out, 0, DEVICE_SECRET_LEN);

    Preferences prefs;
    if (!prefs.begin(NVS_NAMESPACE, /*readOnly=*/false)) {
        ESP_LOGE(TAG, "NVS unavailable - cannot read or create the device key");
        return false;
    }

    size_t got = prefs.getBytes(NVS_KEY, s_key, sizeof(s_key));
    if (got != DEVICE_SECRET_LEN) {
        // First boot on this device (or NVS was erased). Generate one and keep
        // it: every fingerprint already on the SD card was made under a
        // different key and will read as unrecognised, which is the documented
        // consequence of moving a card between devices.
        esp_fill_random(s_key, sizeof(s_key));
        const size_t wrote = prefs.putBytes(NVS_KEY, s_key, sizeof(s_key));
        if (wrote != DEVICE_SECRET_LEN) {
            ESP_LOGE(TAG, "could not persist the device key (wrote %u of %u)", (unsigned)wrote,
                     (unsigned)DEVICE_SECRET_LEN);
            prefs.end();
            memset(s_key, 0, sizeof(s_key));
            return false;  // fail closed: never fingerprint under a key that won't survive
        }
        // Deliberately says nothing about the key itself.
        ESP_LOGW(TAG, "generated a new device key; card bindings from another device "
                      "or an erased NVS will not be recognised");
    }
    prefs.end();

    s_have_key = true;
    memcpy(out, s_key, DEVICE_SECRET_LEN);
    return true;
}

#endif  // NATIVE_TEST
