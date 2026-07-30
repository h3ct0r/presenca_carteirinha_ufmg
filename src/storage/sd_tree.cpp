#include "storage/sd_tree.h"

#include <FS.h>
#include <SD_MMC.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp32-hal-log.h"

static const char* TAG = "sdtree";

// Deep enough for every tree this device writes (/students/checkins/<id>/<file>
// is the deepest at 4), shallow enough that the recursion below can't exhaust
// the LVGL thread's stack.
static constexpr int MAX_DEPTH = 8;
static constexpr size_t MAX_PATH = 128;

// Characters FAT rejects outright, plus the path separator (a rename never
// moves an entry between directories).
static const char* ILLEGAL_NAME_CHARS = "/\\:*?\"<>|";
static constexpr size_t MAX_NAME = 64;

static void set_err(char* err, size_t cap, const char* fmt, ...) {
    if (!err || cap == 0) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, cap, fmt, ap);
    va_end(ap);
}

// Strips a trailing '/' (except on the root) so joins don't produce "//".
static void normalize(const char* path, char* out, size_t cap) {
    snprintf(out, cap, "%s", path ? path : "");
    size_t n = strlen(out);
    while (n > 1 && out[n - 1] == '/') out[--n] = '\0';
}

static bool join(const char* dir, const char* name, char* out, size_t cap) {
    int n = (strcmp(dir, "/") == 0) ? snprintf(out, cap, "/%s", name)
                                    : snprintf(out, cap, "%s/%s", dir, name);
    return n > 0 && (size_t)n < cap;
}

bool sd_tree_is_dir(const char* path) {
    if (!path || !path[0]) return false;
    File f = SD_MMC.open(path);
    bool is_dir = f && f.isDirectory();
    if (f) f.close();
    return is_dir;
}

// Full path of the first entry in `dir`, or false when it is empty/unreadable.
// The directory handle is closed before returning: deleting entries while a
// directory is open is unsafe on this core (same reason attendance_clear lists
// its dates before removing any file).
static bool first_child(const char* dir, char* out, size_t cap) {
    File d = SD_MMC.open(dir);
    if (!d || !d.isDirectory()) {
        if (d) d.close();
        return false;
    }
    File e = d.openNextFile();
    bool got = false;
    if (e) {
        got = join(dir, e.name(), out, cap);  // name() is a basename on this core
        e.close();
    }
    d.close();
    return got;
}

static bool remove_tree(const char* path, int depth, sd_tree_stats_t* st) {
    if (!sd_tree_is_dir(path)) {
        if (SD_MMC.remove(path)) {
            st->removed++;
            return true;
        }
        ESP_LOGE(TAG, "could not delete file %s", path);
        st->failed++;
        return false;
    }

    if (depth >= MAX_DEPTH) {
        ESP_LOGE(TAG, "depth limit reached at %s", path);
        st->failed++;
        return false;
    }

    // Empty the directory one entry at a time, re-opening it for each: the
    // child list changes under us as we delete, so we never hold an open
    // directory across a removal. A child that won't go stops the pass instead
    // of looping forever on the same entry.
    char child[MAX_PATH];
    while (first_child(path, child, sizeof(child))) {
        if (!remove_tree(child, depth + 1, st)) return false;
    }

    if (SD_MMC.rmdir(path)) {
        st->removed++;
        return true;
    }
    ESP_LOGE(TAG, "could not delete directory %s", path);
    st->failed++;
    return false;
}

bool sd_tree_remove(const char* path, sd_tree_stats_t* out, char* err, size_t err_cap) {
    sd_tree_stats_t local = {0, 0};
    sd_tree_stats_t* st = out ? out : &local;
    st->removed = 0;
    st->failed = 0;

    char target[MAX_PATH];
    normalize(path, target, sizeof(target));
    if (target[0] != '/') {
        set_err(err, err_cap, "path must be absolute");
        return false;
    }
    if (strcmp(target, "/") == 0) {
        set_err(err, err_cap, "refusing to delete the card root");
        return false;
    }
    if (!SD_MMC.exists(target)) {
        set_err(err, err_cap, "not found");
        return false;
    }

    bool ok = remove_tree(target, 0, st);
    if (!ok) set_err(err, err_cap, "deleted %d, failed %d", st->removed, st->failed);
    ESP_LOGI(TAG, "remove %s: %d deleted, %d failed", target, st->removed, st->failed);
    return ok;
}

static bool name_is_valid(const char* name, char* err, size_t err_cap) {
    if (!name || !name[0]) {
        set_err(err, err_cap, "name is empty");
        return false;
    }
    if (strlen(name) >= MAX_NAME) {
        set_err(err, err_cap, "name is too long (max %d)", (int)MAX_NAME - 1);
        return false;
    }
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
        set_err(err, err_cap, "\"%s\" is not a name", name);
        return false;
    }
    for (const char* c = name; *c; c++) {
        if ((unsigned char)*c < 0x20 || strchr(ILLEGAL_NAME_CHARS, *c)) {
            set_err(err, err_cap, "name may not contain %s", ILLEGAL_NAME_CHARS);
            return false;
        }
    }
    return true;
}

bool sd_tree_rename(const char* path, const char* new_name, char* err, size_t err_cap) {
    char src[MAX_PATH];
    normalize(path, src, sizeof(src));
    if (src[0] != '/') {
        set_err(err, err_cap, "path must be absolute");
        return false;
    }
    if (strcmp(src, "/") == 0) {
        set_err(err, err_cap, "cannot rename the card root");
        return false;
    }
    if (!name_is_valid(new_name, err, err_cap)) return false;
    if (!SD_MMC.exists(src)) {
        set_err(err, err_cap, "not found");
        return false;
    }

    char parent[MAX_PATH];
    snprintf(parent, sizeof(parent), "%s", src);
    char* slash = strrchr(parent, '/');
    if (slash == parent) {
        parent[1] = '\0';  // entry sits in the root
    } else {
        *slash = '\0';
    }

    char dest[MAX_PATH];
    if (!join(parent, new_name, dest, sizeof(dest))) {
        set_err(err, err_cap, "resulting path is too long");
        return false;
    }
    if (strcmp(dest, src) == 0) return true;  // no-op
    if (SD_MMC.exists(dest)) {
        set_err(err, err_cap, "\"%s\" already exists", new_name);
        return false;
    }
    if (!SD_MMC.rename(src, dest)) {
        set_err(err, err_cap, "rename failed");
        ESP_LOGE(TAG, "rename %s -> %s failed", src, dest);
        return false;
    }
    ESP_LOGI(TAG, "rename %s -> %s", src, dest);
    return true;
}
