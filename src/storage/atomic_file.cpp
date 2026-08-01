#include "storage/atomic_file.h"

#include <FS.h>
#include <SD_MMC.h>
#include <stdio.h>

#include "esp32-hal-log.h"

static const char* TAG = "atomicfile";

// Longest path this is used with is "/students/students.json" (23) plus the
// ".tmp"/".bak" suffix, so 128 is generous. Paths that do not fit are refused
// rather than silently truncated into a path pointing somewhere else.
static constexpr size_t MAX_PATH = 128;

static bool suffixed(const char* path, const char* suffix, char* out, size_t cap) {
    int n = snprintf(out, cap, "%s%s", path, suffix);
    return n > 0 && (size_t)n < cap;
}

bool atomic_file_recover(const char* path) {
    if (!path || !path[0]) return false;
    char bak[MAX_PATH];
    if (!suffixed(path, ".bak", bak, sizeof(bak))) return false;
    if (!SD_MMC.exists(bak)) return false;

    if (SD_MMC.exists(path)) {
        // Interrupted between steps 3 and 4: the new file is already in place,
        // so the backup is just leftovers.
        SD_MMC.remove(bak);
        return false;
    }
    // Interrupted between 2 and 3: the rename never happened and this backup is
    // the only copy of the file. Loud, because it means a write was cut short.
    if (!SD_MMC.rename(bak, path)) {
        ESP_LOGE(TAG, "%s is missing and %s could not be restored", path, bak);
        return false;
    }
    ESP_LOGW(TAG, "recovered %s from %s (a previous write was interrupted)", path, bak);
    return true;
}

bool atomic_file_write(const char* path, const uint8_t* data, size_t len, const char* what) {
    if (!path || !path[0] || (!data && len)) return false;

    char tmp[MAX_PATH], bak[MAX_PATH];
    if (!suffixed(path, ".tmp", tmp, sizeof(tmp)) ||
        !suffixed(path, ".bak", bak, sizeof(bak))) {
        ESP_LOGE(TAG, "write %s: path too long", path);
        return false;
    }

    // 1. Write the replacement alongside the original, which stays untouched.
    File f = SD_MMC.open(tmp, FILE_WRITE, true);
    if (!f) {
        ESP_LOGE(TAG, "write %s: cannot create %s (SD not mounted or write-protected)", what,
                 tmp);
        return false;
    }
    size_t written = len ? f.write(data, len) : 0;
    f.close();
    if (written != len) {
        // A short write on FAT almost always means the card is full.
        ESP_LOGE(TAG, "write %s: short write (%u of %u bytes) - SD card full?", what,
                 (unsigned)written, (unsigned)len);
        SD_MMC.remove(tmp);  // don't leave a truncated .tmp behind
        return false;
    }

    // Any .bak still here is from an earlier interrupted write that
    // atomic_file_recover() already resolved; it must not block step 2.
    SD_MMC.remove(bak);

    // 2. Step the original aside rather than deleting it. This is the whole
    //    point: from here until step 3 the old content is still on the card.
    const bool had_original = SD_MMC.exists(path);
    if (had_original && !SD_MMC.rename(path, bak)) {
        ESP_LOGE(TAG, "write %s: cannot move %s aside", what, path);
        SD_MMC.remove(tmp);
        return false;
    }

    // 3. Put the replacement in place.
    if (!SD_MMC.rename(tmp, path)) {
        ESP_LOGE(TAG, "write %s: rename from %s failed", what, tmp);
        // Undo step 2 so the caller is left with the original, not with nothing.
        if (had_original && !SD_MMC.rename(bak, path)) {
            ESP_LOGE(TAG, "write %s: could not restore %s either - it is at %s", what, path,
                     bak);
        }
        SD_MMC.remove(tmp);
        return false;
    }

    // 4. The replacement is committed; the backup has done its job.
    if (had_original) SD_MMC.remove(bak);
    return true;
}
