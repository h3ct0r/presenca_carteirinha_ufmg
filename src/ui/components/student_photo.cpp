#include "ui/components/student_photo.h"

#include <SD_MMC.h>
#include <stdio.h>

#include "esp32-hal-log.h"
#include "storage/sd_card.h"

static const char* TAG = "student_photo";

// An avatar is decoded by TJPGD into the 128 KB LVGL heap as w*h*2 bytes, so an
// oversized file is not a cosmetic problem — it exhausts the heap, and this
// build turns that into a SILENT infinite loop on the LVGL thread
// (LV_USE_ASSERT_MALLOC + LV_ASSERT_HANDLER `while(1);`). The UI freezes with no
// panic and no reboot while other tasks keep logging normally.
//
// Authored avatars are 100x100 baseline JPEGs (~3-8 KB, see STUDENT_PHOTOS.md).
// 64 KB is ~10x the expected size and still decodes well within the heap, so
// anything above it is a mis-sized file, not a legitimate avatar: skip it and
// say so, rather than hanging the device.
static constexpr size_t AVATAR_MAX_BYTES = 64 * 1024;

bool student_photo_exists(const char* student_id) {
    if (!student_id || !student_id[0] || !sd_card_mount()) return false;
    char path[80];
    snprintf(path, sizeof(path), "/students/photos/%s.jpg", student_id);
    return SD_MMC.exists(path);
}

bool student_photo_src(const char* student_id, char* out, size_t cap) {
    if (!out || !cap) return false;
    out[0] = '\0';
    if (!student_photo_exists(student_id)) return false;

    char path[80];
    snprintf(path, sizeof(path), "/students/photos/%s.jpg", student_id);
    File f = SD_MMC.open(path, FILE_READ);
    if (!f) {
        ESP_LOGW(TAG, "%s exists but cannot be opened — using the placeholder", path);
        return false;
    }
    size_t sz = f.size();
    f.close();
    if (sz == 0 || sz > AVATAR_MAX_BYTES) {
        ESP_LOGE(TAG,
                 "%s is %u bytes (limit %u) — refusing to decode it; re-export the "
                 "avatars at 100x100 (see STUDENT_PHOTOS.md). Using the placeholder.",
                 path, (unsigned)sz, (unsigned)AVATAR_MAX_BYTES);
        return false;
    }

    snprintf(out, cap, "S:/students/photos/%s.jpg", student_id);  // LVGL drive-letter form
    return true;
}
