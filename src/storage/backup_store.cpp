#include "storage/backup_store.h"

#include <FS.h>
#include <SD_MMC.h>
#include <stdio.h>
#include <string.h>

#include "esp32-hal-log.h"
#include "storage/sd_card.h"

static const char* TAG = "backup";

static constexpr const char* BK_PARENT = "/backup";
static constexpr const char* BK_ROOT = "/backup/previous";

// FAT has no recursive mkdir; create each level. mkdir on an existing dir just
// fails harmlessly, so the return is ignored.
static void ensure_dir(const char* path) { SD_MMC.mkdir(path); }

// Copies src -> dst atomically (write dst.tmp, then remove+rename over dst) so a
// power cut leaves either the old file or the new one. Streams in small chunks
// to keep RAM flat even for a ~200 KB students.json. Returns false on any I/O
// failure; a missing source is the caller's concern (checked before calling).
static bool copy_file(const char* src, const char* dst) {
    File in = SD_MMC.open(src, FILE_READ);
    if (!in) return false;

    char tmp[192];
    snprintf(tmp, sizeof(tmp), "%s.tmp", dst);
    File out = SD_MMC.open(tmp, FILE_WRITE, true);
    if (!out) {
        in.close();
        return false;
    }

    uint8_t chunk[512];
    bool ok = true;
    for (;;) {
        size_t n = in.readBytes((char*)chunk, sizeof(chunk));
        if (n == 0) break;
        if (out.write(chunk, n) != n) {
            ok = false;
            break;
        }
    }
    in.close();
    out.close();
    if (!ok) {
        SD_MMC.remove(tmp);
        return false;
    }
    SD_MMC.remove(dst);  // FAT rename needs the target absent
    return SD_MMC.rename(tmp, dst);
}

backup_result_t backup_store_create(void) {
    backup_result_t r = {false, ""};
    if (!sd_card_mount()) {
        snprintf(r.message, sizeof(r.message), "No SD card");
        return r;
    }

    ensure_dir(BK_PARENT);
    ensure_dir(BK_ROOT);
    int copied = 0;

    if (SD_MMC.exists("/config.json")) {
        if (!copy_file("/config.json", "/backup/previous/config.json")) {
            snprintf(r.message, sizeof(r.message), "Backup of config.json failed");
            return r;
        }
        copied++;
    }

    if (SD_MMC.exists("/students/students.json")) {
        ensure_dir("/backup/previous/students");
        if (!copy_file("/students/students.json", "/backup/previous/students/students.json")) {
            snprintf(r.message, sizeof(r.message), "Backup of students.json failed");
            return r;
        }
        copied++;
    }

    // classes/<CODE>/class.json — scan the directory, not the loaded roster.
    File dir = SD_MMC.open("/classes");
    if (dir && dir.isDirectory()) {
        ensure_dir("/backup/previous/classes");
        File e;
        while ((e = dir.openNextFile())) {
            if (!e.isDirectory()) {
                e.close();
                continue;
            }
            char dname[40];
            snprintf(dname, sizeof(dname), "%s", e.name());
            e.close();

            char src[128];
            snprintf(src, sizeof(src), "/classes/%s/class.json", dname);
            if (!SD_MMC.exists(src)) continue;  // a dir without class.json — skip

            char dstdir[160], dst[192];
            snprintf(dstdir, sizeof(dstdir), "/backup/previous/classes/%s", dname);
            ensure_dir(dstdir);
            snprintf(dst, sizeof(dst), "%s/class.json", dstdir);
            if (!copy_file(src, dst)) {
                dir.close();
                snprintf(r.message, sizeof(r.message), "Backup of class %s failed", dname);
                return r;
            }
            copied++;
        }
        dir.close();
    }

    ESP_LOGI(TAG, "snapshot created: %d file(s) into %s", copied, BK_ROOT);
    r.ok = true;
    snprintf(r.message, sizeof(r.message), "Backed up %d file(s)", copied);
    return r;
}

bool backup_store_exists(void) { return SD_MMC.exists("/backup/previous/config.json"); }

const char* backup_store_root(void) { return BK_ROOT; }
