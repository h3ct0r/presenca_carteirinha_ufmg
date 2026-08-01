#include "storage/sd_tree.h"

#include <FS.h>
#include <SD_MMC.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>  // strcasecmp

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

// How many child NAMES one directory scan collects before we start deleting.
// Taking a single name per scan made emptying a directory O(entries^2):
// /students/photos with 600 avatars cost ~180,000 directory-entry reads for one
// wipe. A batch amortises the scan while still never holding a directory handle
// across a removal — which is the part that is actually unsafe.
//
// Names, not full paths, and a modest batch, because this buffer is per
// recursion level: CHILD_BATCH * MAX_NAME * MAX_DEPTH is the worst-case stack
// (12 * 64 * 8 ≈ 6 KB) and this runs on the 16 KB LVGL thread.
static constexpr int CHILD_BATCH = 12;

// Up to CHILD_BATCH child names of `dir`; returns how many were filled. The
// directory handle is closed before returning: deleting entries while a
// directory is open is unsafe on this core (same reason attendance_clear lists
// its dates before removing any file).
static int next_children(const char* dir, char out[][MAX_NAME], int max) {
    File d = SD_MMC.open(dir);
    if (!d || !d.isDirectory()) {
        if (d) d.close();
        return 0;
    }
    int n = 0;
    File e;
    while (n < max && (e = d.openNextFile())) {
        const char* name = e.name();  // basename on this core
        // Skip rather than truncate: a truncated name is a path pointing at
        // something else. A directory of only such names yields 0 here, and the
        // caller's rmdir then fails and reports it — no infinite loop.
        if (strlen(name) < MAX_NAME) {
            snprintf(out[n++], MAX_NAME, "%s", name);
        } else {
            ESP_LOGE(TAG, "entry name too long to delete under %s: %s", dir, name);
        }
        e.close();
    }
    d.close();
    return n;
}

// `cb` may be NULL. Reported after each successful removal, with the running
// `removed` count as `done` — the total is unknowable until the walk finishes.
static void report(progress_cb_t cb, void* ctx, const sd_tree_stats_t* st, const char* path) {
    if (!cb) return;
    progress_t p = {"Deleting", path, st->removed, 0};
    cb(&p, ctx);
}

static bool remove_tree(const char* path, int depth, sd_tree_stats_t* st, progress_cb_t cb,
                        void* ctx) {
    if (!sd_tree_is_dir(path)) {
        if (SD_MMC.remove(path)) {
            st->removed++;
            report(cb, ctx, st, path);
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

    // Empty the directory a batch at a time, re-opening it for each batch: the
    // child list changes under us as we delete, so we never hold an open
    // directory across a removal. A child that won't go stops the pass instead
    // of looping forever on the same entry.
    //
    // The batch is what keeps this linear; see CHILD_BATCH for the stack budget.
    for (;;) {
        char names[CHILD_BATCH][MAX_NAME];
        const int n = next_children(path, names, CHILD_BATCH);
        if (n == 0) break;  // empty, unreadable, or nothing usable left
        for (int i = 0; i < n; i++) {
            char child[MAX_PATH];
            if (!join(path, names[i], child, sizeof(child))) {
                ESP_LOGE(TAG, "path too long to delete: %s/%s", path, names[i]);
                st->failed++;
                return false;
            }
            if (!remove_tree(child, depth + 1, st, cb, ctx)) return false;
        }
    }

    if (SD_MMC.rmdir(path)) {
        st->removed++;
        report(cb, ctx, st, path);
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

    // No callback: the web file manager's own task is the only caller (sd_tree.h).
    bool ok = remove_tree(target, 0, st, nullptr, nullptr);
    if (!ok) set_err(err, err_cap, "deleted %d, failed %d", st->removed, st->failed);
    ESP_LOGI(TAG, "remove %s: %d deleted, %d failed", target, st->removed, st->failed);
    return ok;
}

// Safety net on the delete loop below, not a limit on the card: an entry that
// refuses to be deleted would otherwise be handed back by every rescan forever.
// A device card holds ~6 root entries.
static constexpr int MAX_ROOT_ENTRIES = 64;

// Full path of the first root entry that is not `keep`, or false when nothing
// else is left. Closes the directory before returning, for the same reason
// first_child() does.
static bool first_root_victim(const char* keep, char* out, size_t cap) {
    File root = SD_MMC.open("/");
    if (!root || !root.isDirectory()) {
        if (root) root.close();
        return false;
    }
    bool got = false;
    File e;
    while (!got && (e = root.openNextFile())) {
        const char* name = e.name();  // basename on this core
        // FAT is case-insensitive, so CONFIG.JSON *is* config.json.
        if (!(keep && keep[0] && strcasecmp(name, keep) == 0)) {
            got = join("/", name, out, cap);
            if (!got) ESP_LOGE(TAG, "root entry name too long to delete: %s", name);
        }
        e.close();
    }
    root.close();
    return got;
}

bool sd_tree_wipe_root(const char* keep, sd_tree_stats_t* out, char* err, size_t err_cap,
                       progress_cb_t cb, void* ctx) {
    sd_tree_stats_t local = {0, 0};
    sd_tree_stats_t* st = out ? out : &local;
    st->removed = 0;
    st->failed = 0;

    // Probe the root once so "unreadable card" is reported as itself rather
    // than as the empty-root success the loop below would otherwise return.
    File root = SD_MMC.open("/");
    bool readable = root && root.isDirectory();
    if (root) root.close();
    if (!readable) {
        set_err(err, err_cap, "cannot open the card root");
        return false;
    }

    // One entry at a time, re-scanning the root for each: deleting entries
    // while a directory handle is open is unsafe on this core, and this way the
    // listing needs no buffer (a 4 KB static here was enough to push internal
    // RAM past a linker region).
    int guard = 0;
    for (; guard < MAX_ROOT_ENTRIES; guard++) {
        char victim[MAX_PATH];
        if (!first_root_victim(keep, victim, sizeof(victim))) break;  // nothing left
        if (!remove_tree(victim, 0, st, cb, ctx)) {
            // Stop rather than rescan: the next pass would return this same
            // undeletable entry and spin.
            set_err(err, err_cap, "deleted %d, failed on %s", st->removed, victim);
            ESP_LOGE(TAG, "wipe root stopped at %s (%d deleted)", victim, st->removed);
            return false;
        }
    }

    ESP_LOGI(TAG, "wipe root (keeping %s): %d deleted", keep && keep[0] ? keep : "-",
             st->removed);
    if (guard >= MAX_ROOT_ENTRIES) {
        set_err(err, err_cap, "more than %d root entries - deleted %d, run it again",
                MAX_ROOT_ENTRIES, st->removed);
        return false;
    }
    return true;
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
