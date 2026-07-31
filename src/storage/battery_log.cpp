#include "storage/battery_log.h"

#include <FS.h>
#include <SD_MMC.h>
#include <stdio.h>
#include <string.h>

#include "esp32-hal-log.h"

static const char* TAG = "batlog";

static const char* LOG_PATH = "/battery.csv";
static const char* LOG_HEADER = "uptime_s,mv,pct\n";

const char* battery_log_path(void) { return LOG_PATH; }

bool battery_log_append(uint32_t uptime_s, uint16_t mv, uint8_t pct) {
    // Checked before opening: opening in append mode creates the file, after
    // which "is it new?" can no longer be asked.
    const bool fresh = !SD_MMC.exists(LOG_PATH);

    File f = SD_MMC.open(LOG_PATH, FILE_APPEND, true);
    if (!f) {
        ESP_LOGE(TAG, "cannot open %s", LOG_PATH);
        return false;
    }

    char line[48];
    int len = snprintf(line, sizeof(line), "%u,%u,%u\n", (unsigned)uptime_s, (unsigned)mv,
                       (unsigned)pct);
    bool ok = true;
    if (fresh) {
        size_t hn = strlen(LOG_HEADER);
        ok = f.write((const uint8_t*)LOG_HEADER, hn) == hn;
    }
    if (ok) ok = f.write((const uint8_t*)line, len) == (size_t)len;
    f.close();

    if (!ok) ESP_LOGE(TAG, "write to %s failed (card full?)", LOG_PATH);
    return ok;
}
