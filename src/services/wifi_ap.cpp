#include "services/wifi_ap.h"

#include <WiFi.h>
#include <esp_heap_caps.h>
#include <esp_random.h>
#include <stdio.h>
#include <string.h>

#include "esp32-hal-log.h"

static const char* TAG = "wifi_ap";

// Bringing the AP up starts ESP-Hosted and the WiFi stack, which take a large
// and PERMANENT bite out of internal RAM — the same pool sdmmc needs a
// DMA-capable buffer from for every transfer. When it ran out, every SD read
// failed with ESP_ERR_NO_MEM and ESP-Hosted itself eventually asserted in
// transport_drv_ap_tx. These lines are how that budget gets watched: compare
// "before" with "after", and with the boot figure main.cpp prints.
static void log_heap(const char* when) {
    ESP_LOGI(TAG, "%s: internal %u B free (largest %u), DMA %u B, PSRAM %u B", when,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

static char s_ssid[33] = "";
static char s_pass[16] = "";
static bool s_running = false;
static bool s_was_started = false;  // sticky, for the log label below

// Generates the SSID/password once per boot so they stay stable while the
// screen is open (and across revisits).
static void generate_once(void) {
    if (s_ssid[0]) return;
    snprintf(s_ssid, sizeof(s_ssid), "CARTEIRINHA-%04X", (unsigned)(esp_random() & 0xFFFF));
    for (int i = 0; i < 8; i++) s_pass[i] = (char)('0' + (esp_random() % 10));
    s_pass[8] = '\0';
    ESP_LOGI(TAG, "generated AP creds: %s / %s", s_ssid, s_pass);
}

void wifi_ap_credentials(char* ssid, size_t ssid_cap, char* pass, size_t pass_cap) {
    generate_once();
    if (ssid && ssid_cap) snprintf(ssid, ssid_cap, "%s", s_ssid);
    if (pass && pass_cap) snprintf(pass, pass_cap, "%s", s_pass);
}

bool wifi_ap_start(void) {
    generate_once();
    log_heap(s_was_started ? "before AP restart" : "before first AP start");
    WiFi.mode(WIFI_AP);
    bool ok = WiFi.softAP(s_ssid, s_pass);
    if (!ok) {
        // Benign retry (no radio teardown — that would drop the C6 link).
        ESP_LOGW(TAG, "soft-AP first attempt failed, retrying");
        delay(200);
        ok = WiFi.softAP(s_ssid, s_pass);
    }
    s_running = ok;
    if (ok) {
        s_was_started = true;
        ESP_LOGI(TAG, "soft-AP up: %s ip=%s", s_ssid, WiFi.softAPIP().toString().c_str());
    } else {
        ESP_LOGE(TAG, "soft-AP failed to start (C6 companion available?)");
    }
    log_heap("after AP start");
    return ok;
}

void wifi_ap_stop(void) {
    // Only bring the AP config down (empty SSID stops the visible network);
    // keep the WiFi stack and the P4<->C6 link initialized. Fully powering the
    // radio off (softAPdisconnect(true) / WIFI_OFF) tears down that link, and
    // re-establishing it on the next start() is what made restart fail.
    WiFi.softAPdisconnect(false);
    s_running = false;
    ESP_LOGI(TAG, "soft-AP stopped (stack kept alive for reliable restart)");
    // Expected to be close to "after AP start", not to the boot figure: this
    // only drops the visible network, it does not free the stack.
    log_heap("after AP stop");
}

bool wifi_ap_is_running(void) { return s_running; }

void wifi_ap_ip(char* out, size_t cap) {
    if (!cap) return;
    if (!s_running) {
        out[0] = '\0';
        return;
    }
    snprintf(out, cap, "%s", WiFi.softAPIP().toString().c_str());
}
