#include "services/wifi_ap.h"

#include <WiFi.h>
#include <esp_random.h>
#include <stdio.h>
#include <string.h>

#include "esp32-hal-log.h"

static const char* TAG = "wifi_ap";

static char s_ssid[33] = "";
static char s_pass[16] = "";
static bool s_running = false;

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
        ESP_LOGI(TAG, "soft-AP up: %s ip=%s", s_ssid, WiFi.softAPIP().toString().c_str());
    } else {
        ESP_LOGE(TAG, "soft-AP failed to start (C6 companion available?)");
    }
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
