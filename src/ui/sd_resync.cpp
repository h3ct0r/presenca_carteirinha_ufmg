#include "ui/sd_resync.h"

#include "esp32-hal-log.h"
#include "services/config_service.h"
#include "services/file_server.h"
#include "services/roster_service.h"
#include "storage/attendance_store.h"

static const char* TAG = "sd_resync";

// One last-seen counter per scope. They must be separate: a light resync on the
// class screen would otherwise swallow the change and the class list would
// never learn that the roster moved.
static uint32_t s_seen_light = 0;
static uint32_t s_seen_full = 0;

// True (and stores the new value) only when the web editor wrote since the last
// call for this scope.
static bool changed(uint32_t* seen) {
    uint32_t now = file_server_write_count();
    if (now == *seen) return false;
    *seen = now;
    return true;
}

void ui_sd_resync_light(void) {
    if (!changed(&s_seen_light)) return;
    // Present-counts are memoised per (class, date) and nothing in
    // attendance_store saw these edits.
    attendance_history_cache_clear();
    ESP_LOGI(TAG, "attendance cache dropped after a web edit");
}

void ui_sd_resync_full(void) {
    ui_sd_resync_light();
    if (!changed(&s_seen_full)) return;
    // Both reload synchronously from the card, so this is the expensive half —
    // hence only on screens that need the data fresh anyway.
    config_service_reload();
    roster_service_reload();
    ESP_LOGI(TAG, "config + roster reloaded after a web edit");
}
