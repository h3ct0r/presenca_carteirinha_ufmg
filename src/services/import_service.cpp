#include "services/import_service.h"

#include <FS.h>
#include <SD_MMC.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app/ustar.h"
#include "esp32-hal-log.h"
#include "services/config_service.h"
#include "services/roster_service.h"
#include "storage/backup_store.h"
#include "storage/sd_card.h"

static const char* TAG = "import";

static constexpr const char* SD_TAR = "/config.tar";
static constexpr const char* SENTINEL = "/config.tar.imported";
static constexpr const char* STAGING = "/import_staging";
static constexpr size_t MAX_TAR = 16 * 1024 * 1024;  // 16 MB cap (CONFIG_IMPORT.md §4)

const char* import_service_tar_path(void) { return SD_TAR; }

bool import_service_pending(void) { return sd_card_mount() && SD_MMC.exists(SD_TAR); }

static const char* ustar_err_str(ustar_status_t s) {
    switch (s) {
        case USTAR_OK: return "ok";
        case USTAR_ERR_TRUNCATED: return "archive truncated";
        case USTAR_ERR_MAGIC: return "not a tar archive";
        case USTAR_ERR_CHECKSUM: return "corrupt header";
        case USTAR_ERR_NAME: return "disallowed file path";
        case USTAR_ERR_TOO_BIG: return "archive too large";
        case USTAR_ERR_ABORT: return "aborted";
    }
    return "unknown";
}

// --- small filesystem helpers -----------------------------------------------

static void ensure_dir(const char* path) { SD_MMC.mkdir(path); }

// mkdir every parent directory of `path` (FAT has no recursive mkdir).
static void mkparents(const char* path) {
    char buf[192];
    snprintf(buf, sizeof(buf), "%s", path);
    for (char* p = buf + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            SD_MMC.mkdir(buf);
            *p = '/';
        }
    }
}

// Atomic copy: write dst.tmp, then remove+rename over dst. Streams in chunks.
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
    SD_MMC.remove(dst);
    return SD_MMC.rename(tmp, dst);
}

// Removes every regular file directly inside `dir` (non-recursive). Used to
// clear a staged tree's leaf files; empty dirs left behind are harmless.
static void remove_dir_files(const char* dir) {
    File d = SD_MMC.open(dir);
    if (!d || !d.isDirectory()) {
        if (d) d.close();
        return;
    }
    File e;
    while ((e = d.openNextFile())) {
        bool is_dir = e.isDirectory();
        char p[192];
        snprintf(p, sizeof(p), "%s/%s", dir, e.name());
        e.close();
        if (!is_dir) SD_MMC.remove(p);
    }
    d.close();
}

// Removes the staged authored files so a stale class from a previous import
// can't be applied. Leaves empty dirs (harmless; apply skips missing class.json).
static void clean_staging(void) {
    SD_MMC.remove("/import_staging/config.json");
    SD_MMC.remove("/import_staging/students/students.json");
    remove_dir_files("/import_staging/students/photos");
    File dir = SD_MMC.open("/import_staging/classes");
    if (dir && dir.isDirectory()) {
        File e;
        while ((e = dir.openNextFile())) {
            if (e.isDirectory()) {
                char p[192];
                snprintf(p, sizeof(p), "/import_staging/classes/%s/class.json", e.name());
                e.close();
                SD_MMC.remove(p);
            } else {
                e.close();
            }
        }
        dir.close();
    }
}

// --- unpack -----------------------------------------------------------------

struct unpack_ctx {
    bool ok;
    char msg[96];
};

static bool unpack_visitor(const ustar_entry_t* e, void* ctx) {
    unpack_ctx* u = (unpack_ctx*)ctx;
    char dst[192];
    snprintf(dst, sizeof(dst), "%s/%s", STAGING, e->name);
    mkparents(dst);
    File out = SD_MMC.open(dst, FILE_WRITE, true);
    if (!out) {
        u->ok = false;
        snprintf(u->msg, sizeof(u->msg), "cannot write staging file");
        return false;
    }
    size_t w = e->size ? out.write(e->data, e->size) : 0;
    out.close();
    if (e->size && w != e->size) {
        u->ok = false;
        snprintf(u->msg, sizeof(u->msg), "short write to staging");
        return false;
    }
    return true;
}

// --- apply ------------------------------------------------------------------

// Copies each authored file from a validated tree at `root` into place, atomic
// per file. attendance/photos/models are never named here, so they survive.
static bool apply_tree(const char* root, char* err, size_t cap) {
    char src[192];

    snprintf(src, sizeof(src), "%s/config.json", root);
    if (!copy_file(src, "/config.json")) {
        snprintf(err, cap, "Apply of config.json failed");
        return false;
    }

    ensure_dir("/students");
    snprintf(src, sizeof(src), "%s/students/students.json", root);
    if (!copy_file(src, "/students/students.json")) {
        snprintf(err, cap, "Apply of students.json failed");
        return false;
    }

    // Optional authored avatars: overwrite each staged students/photos/<id>.jpg
    // into place (dir created if new). Absent tree -> nothing to do. The device's
    // own /photos/** and /students/checkins/** are never named here (preserved).
    char photos_src[160];
    snprintf(photos_src, sizeof(photos_src), "%s/students/photos", root);
    File pdir = SD_MMC.open(photos_src);
    if (pdir && pdir.isDirectory()) {
        ensure_dir("/students/photos");
        File pe;
        while ((pe = pdir.openNextFile())) {
            if (pe.isDirectory()) {
                pe.close();
                continue;
            }
            char fname[64];
            snprintf(fname, sizeof(fname), "%s", pe.name());
            pe.close();
            char psrc[192], pdst[160];
            snprintf(psrc, sizeof(psrc), "%s/%s", photos_src, fname);
            snprintf(pdst, sizeof(pdst), "/students/photos/%s", fname);
            if (!copy_file(psrc, pdst)) {
                pdir.close();
                snprintf(err, cap, "Apply of avatar %s failed", fname);
                return false;
            }
        }
        pdir.close();
    }

    char classes_dir[160];
    snprintf(classes_dir, sizeof(classes_dir), "%s/classes", root);
    File dir = SD_MMC.open(classes_dir);
    if (dir && dir.isDirectory()) {
        ensure_dir("/classes");
        File e;
        while ((e = dir.openNextFile())) {
            if (!e.isDirectory()) {
                e.close();
                continue;
            }
            char dname[40];
            snprintf(dname, sizeof(dname), "%s", e.name());
            e.close();
            snprintf(src, sizeof(src), "%s/classes/%s/class.json", root, dname);
            if (!SD_MMC.exists(src)) continue;
            char dstdir[128], dst[192];
            snprintf(dstdir, sizeof(dstdir), "/classes/%s", dname);
            ensure_dir(dstdir);
            snprintf(dst, sizeof(dst), "%s/class.json", dstdir);
            if (!copy_file(src, dst)) {
                dir.close();
                snprintf(err, cap, "Apply of class %s failed", dname);
                return false;
            }
        }
        dir.close();
    }
    return true;
}

// Validates a tree at `root` with the live rules, applies it, and reloads the
// services. Shared by import (root = staging) and revert (root = backup). Does
// NOT back up — the caller owns that decision.
static import_result_t apply_validated_tree(const char* root) {
    import_result_t r = {false, ""};
    char vmsg[128];
    if (!config_validate_tree(root, vmsg, sizeof(vmsg))) {
        snprintf(r.message, sizeof(r.message), "Config invalid: %s", vmsg);
        return r;
    }
    if (!roster_validate_tree(root, vmsg, sizeof(vmsg))) {
        snprintf(r.message, sizeof(r.message), "Roster invalid: %s", vmsg);
        return r;
    }
    if (!apply_tree(root, r.message, sizeof(r.message))) return r;

    config_service_reload();
    roster_service_reload();
    r.ok = true;
    return r;
}

// --- public -----------------------------------------------------------------

import_result_t import_service_run(const char* tar_path) {
    import_result_t r = {false, ""};
    if (!sd_card_mount()) {
        snprintf(r.message, sizeof(r.message), "No SD card");
        return r;
    }

    // 1. Read the staged tar into a heap buffer (capped).
    File f = SD_MMC.open(tar_path, FILE_READ);
    if (!f) {
        snprintf(r.message, sizeof(r.message), "No import file found");
        return r;
    }
    size_t sz = f.size();
    if (sz == 0 || sz > MAX_TAR) {
        f.close();
        snprintf(r.message, sizeof(r.message), sz ? "Import file too large" : "Import file is empty");
        return r;
    }
    uint8_t* buf = (uint8_t*)malloc(sz);
    if (!buf) {
        f.close();
        snprintf(r.message, sizeof(r.message), "Out of memory");
        return r;
    }
    size_t n = f.readBytes((char*)buf, sz);
    f.close();
    if (n != sz) {
        free(buf);
        snprintf(r.message, sizeof(r.message), "Could not read import file");
        return r;
    }

    // 2. Structural + §4 whitelist check before we change anything.
    ustar_status_t us = ustar_iterate(buf, n, MAX_TAR, nullptr, nullptr);
    if (us != USTAR_OK) {
        free(buf);
        snprintf(r.message, sizeof(r.message), "Invalid archive: %s", ustar_err_str(us));
        return r;
    }

    // 3. Back up the current config (safety net) before touching anything.
    backup_result_t bk = backup_store_create();
    if (!bk.ok) {
        free(buf);
        snprintf(r.message, sizeof(r.message), "Backup failed: %s", bk.message);
        return r;
    }

    // 4. Unpack into a clean staging tree.
    clean_staging();
    ensure_dir(STAGING);
    unpack_ctx u = {true, ""};
    us = ustar_iterate(buf, n, MAX_TAR, unpack_visitor, &u);
    free(buf);
    if (us != USTAR_OK || !u.ok) {
        clean_staging();
        snprintf(r.message, sizeof(r.message), "Unpack failed: %s",
                 u.ok ? ustar_err_str(us) : u.msg);
        return r;
    }

    // 5-7. Validate the staging tree, apply, reload.
    r = apply_validated_tree(STAGING);
    clean_staging();
    if (!r.ok) return r;

    // 8. Sentinel the SD source so it isn't re-imported on the next boot.
    if (strcmp(tar_path, SD_TAR) == 0) {
        SD_MMC.remove(SENTINEL);
        SD_MMC.rename(SD_TAR, SENTINEL);
    }

    ESP_LOGI(TAG, "import applied from %s", tar_path);
    snprintf(r.message, sizeof(r.message), "Config imported (previous config backed up)");
    return r;
}

import_result_t import_service_revert(void) {
    import_result_t r = {false, ""};
    if (!sd_card_mount()) {
        snprintf(r.message, sizeof(r.message), "No SD card");
        return r;
    }
    if (!backup_store_exists()) {
        snprintf(r.message, sizeof(r.message), "No backup to restore");
        return r;
    }
    r = apply_validated_tree(backup_store_root());
    if (r.ok) {
        ESP_LOGI(TAG, "reverted to backup snapshot");
        snprintf(r.message, sizeof(r.message), "Restored the previous config");
    }
    return r;
}
