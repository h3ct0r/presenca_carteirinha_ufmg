#include "storage/sd_card.h"

#include <SD_MMC.h>

#include "esp32-hal-log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char* TAG = "sd";

// The esp32p4 variant routes SD_MMC to the SDMMC IOMUX pins (the TF card on
// the schematic) and powers TF_VCC from the chip LDO, so begin() needs no
// pin setup.

static SemaphoreHandle_t s_lock = nullptr;
static bool s_mounted = false;

bool sd_card_mount(void) {
    // First call happens in setup() (single-threaded), so lazily creating the
    // mutex here is race-free.
    if (!s_lock) s_lock = xSemaphoreCreateMutex();

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_mounted) {
        if (SD_MMC.begin("/sdcard")) {
            s_mounted = true;
            ESP_LOGI(TAG, "mounted (%llu MB)", SD_MMC.cardSize() / (1024ULL * 1024ULL));
        } else {
            ESP_LOGW(TAG, "mount failed - no card or not FAT32");
        }
    }
    bool mounted = s_mounted;
    xSemaphoreGive(s_lock);
    return mounted;
}

bool sd_card_is_mounted(void) { return s_mounted; }

bool sd_card_usage(unsigned long long* total_bytes, unsigned long long* used_bytes) {
    if (!sd_card_mount()) return false;
    if (total_bytes) *total_bytes = SD_MMC.totalBytes();
    if (used_bytes) *used_bytes = SD_MMC.usedBytes();
    return true;
}
