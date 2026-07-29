#include "storage/attendance_store.h"

#include <ArduinoJson.h>
#include <FS.h>
#include <SD_MMC.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp32-hal-log.h"

static const char* TAG = "attend";

static constexpr int MAX_PRESENT = 100;  // == ROSTER_MAX_CLASS_STUDENTS
static constexpr int ID_LEN = 20;
static constexpr long long MINUTE_US = 60000000LL;

static char s_dir[24] = "";
static char s_date[12] = "";
static bool s_open = false;

// Present students in the open session, with their measured minutes (0 when
// unknown — single-tap check-ins). Parallel arrays kept in sync.
static char s_present[MAX_PRESENT][ID_LEN];
static int s_present_min[MAX_PRESENT];
static int s_present_count = 0;

// Timed mode: students who tapped in and haven't tapped out. RAM only — a
// reboot voids in-progress measurements (no RTC; monotonic clock resets).
static char s_tapin_id[MAX_PRESENT][ID_LEN];
static long long s_tapin_us[MAX_PRESENT];
static int s_tapin_count = 0;

static void att_path(const char* dir, const char* date, char* out, size_t cap) {
    snprintf(out, cap, "/classes/%s/attendance/%s.jsonl", dir, date);
}

// --- present set (with minutes) ---------------------------------------------

static int present_find(const char* id) {
    for (int i = 0; i < s_present_count; i++) {
        if (strcmp(s_present[i], id) == 0) return i;
    }
    return -1;
}

// Adds/updates (present) or removes (absent) an id, keeping s_present_min aligned.
static void present_set(const char* id, bool present, int minutes) {
    int f = present_find(id);
    if (present) {
        if (f < 0) {
            if (s_present_count >= MAX_PRESENT) return;
            f = s_present_count++;
            snprintf(s_present[f], ID_LEN, "%s", id);
        }
        s_present_min[f] = minutes;
    } else if (f >= 0) {
        int last = --s_present_count;  // swap-remove
        snprintf(s_present[f], ID_LEN, "%s", s_present[last]);
        s_present_min[f] = s_present_min[last];
    }
}

// --- in-progress tap-in set -------------------------------------------------

static int tapin_find(const char* id) {
    for (int i = 0; i < s_tapin_count; i++) {
        if (strcmp(s_tapin_id[i], id) == 0) return i;
    }
    return -1;
}

static void tapin_remove(int f) {
    int last = --s_tapin_count;  // swap-remove
    snprintf(s_tapin_id[f], ID_LEN, "%s", s_tapin_id[last]);
    s_tapin_us[f] = s_tapin_us[last];
}

static void tapin_add(const char* id, long long us) {
    if (s_tapin_count >= MAX_PRESENT) return;
    int i = s_tapin_count++;
    snprintf(s_tapin_id[i], ID_LEN, "%s", id);
    s_tapin_us[i] = us;
}

// --- generic present-only fold (history counts) -----------------------------

static void set_apply(char set[][ID_LEN], int* count, const char* id, bool present) {
    int found = -1;
    for (int i = 0; i < *count; i++) {
        if (strcmp(set[i], id) == 0) {
            found = i;
            break;
        }
    }
    if (present && found < 0 && *count < MAX_PRESENT) {
        snprintf(set[(*count)++], ID_LEN, "%s", id);
    } else if (!present && found >= 0) {
        snprintf(set[found], ID_LEN, "%s", set[*count - 1]);  // swap-remove
        (*count)--;
    }
}

static bool fold_file(const char* path, char set[][ID_LEN], int* count) {
    *count = 0;
    if (!SD_MMC.exists(path)) return true;
    File f = SD_MMC.open(path, FILE_READ);
    if (!f) return false;
    size_t sz = f.size();
    char* buf = (char*)malloc(sz + 1);
    if (!buf) {
        f.close();
        return false;
    }
    size_t n = f.readBytes(buf, sz);
    buf[n] = '\0';
    f.close();

    char* save = nullptr;
    for (char* line = strtok_r(buf, "\n", &save); line; line = strtok_r(nullptr, "\n", &save)) {
        if (line[0] != '{') continue;
        JsonDocument doc;
        if (deserializeJson(doc, line)) continue;
        const char* id = doc["id"] | "";
        bool present = doc["present"] | false;
        if (id[0]) set_apply(set, count, id, present);
    }
    free(buf);
    return true;
}

// Folds the open session's file into s_present / s_present_min (last record per
// id wins, carrying its "min"). In-progress state is NOT persisted, so it starts
// empty on (re)open.
static bool fold_open(const char* path) {
    s_present_count = 0;
    s_tapin_count = 0;
    if (!SD_MMC.exists(path)) return true;
    File f = SD_MMC.open(path, FILE_READ);
    if (!f) return false;
    size_t sz = f.size();
    char* buf = (char*)malloc(sz + 1);
    if (!buf) {
        f.close();
        return false;
    }
    size_t n = f.readBytes(buf, sz);
    buf[n] = '\0';
    f.close();

    char* save = nullptr;
    for (char* line = strtok_r(buf, "\n", &save); line; line = strtok_r(nullptr, "\n", &save)) {
        if (line[0] != '{') continue;
        JsonDocument doc;
        if (deserializeJson(doc, line)) continue;
        const char* id = doc["id"] | "";
        bool present = doc["present"] | false;
        int minutes = doc["min"] | 0;
        if (id[0]) present_set(id, present, minutes);
    }
    free(buf);
    return true;
}

// Appends one JSONL record, optionally with the "min" field.
static bool append_record(const char* id, bool present, int minutes, bool with_min) {
    char adir[64];
    snprintf(adir, sizeof(adir), "/classes/%s/attendance", s_dir);
    if (!SD_MMC.exists(adir)) SD_MMC.mkdir(adir);

    char path[80];
    att_path(s_dir, s_date, path, sizeof(path));
    File f = SD_MMC.open(path, FILE_APPEND, true);
    if (!f) {
        ESP_LOGE(TAG, "append open failed: %s", path);
        return false;
    }
    char line[80];
    int len;
    if (with_min) {
        len = snprintf(line, sizeof(line), "{\"id\":\"%s\",\"present\":%s,\"min\":%d}\n", id,
                       present ? "true" : "false", minutes);
    } else {
        len = snprintf(line, sizeof(line), "{\"id\":\"%s\",\"present\":%s}\n", id,
                       present ? "true" : "false");
    }
    size_t w = f.write((const uint8_t*)line, len);
    f.close();
    return w == (size_t)len;
}

// --- session ----------------------------------------------------------------

bool attendance_open(const char* class_dir, const char* date) {
    attendance_close();
    snprintf(s_dir, sizeof(s_dir), "%s", class_dir);
    snprintf(s_date, sizeof(s_date), "%s", date);
    s_open = true;

    char path[80];
    att_path(class_dir, date, path, sizeof(path));
    bool ok = fold_open(path);
    ESP_LOGI(TAG, "open %s %s: %d present%s", class_dir, date, s_present_count,
             ok ? "" : " (read error)");
    return ok;
}

bool attendance_is_open(void) { return s_open; }
const char* attendance_dir(void) { return s_open ? s_dir : ""; }
const char* attendance_date(void) { return s_open ? s_date : ""; }
bool attendance_is_present(const char* id) { return present_find(id) >= 0; }
int attendance_present_count(void) { return s_present_count; }

bool attendance_set(const char* id, bool present) {
    if (!s_open || !id || !id[0]) return false;
    // A manual override clears any in-progress timing for this student.
    int t = tapin_find(id);
    if (t >= 0) tapin_remove(t);
    present_set(id, present, 0);
    return append_record(id, present, 0, false);
}

// Whole minutes still to wait, rounded up so a student is never told "0 min"
// while the threshold is not actually met yet.
static int remaining_min(long long elapsed_us, int threshold_min) {
    long long need = (long long)threshold_min * MINUTE_US - elapsed_us;
    if (need <= 0) return 0;
    return (int)((need + MINUTE_US - 1) / MINUTE_US);
}

// Microseconds since the arrival tap at index `t`, clamped at 0 (a monotonic
// clock shouldn't go backwards, but a bad `now_us` must not underflow).
static long long tapin_elapsed(int t, long long now_us) {
    long long e = now_us - s_tapin_us[t];
    return e < 0 ? 0 : e;
}

att_state_t attendance_tap(const char* id, long long now_us, int threshold_min) {
    att_state_t r = {ATT_ABSENT, 0, 0};
    if (!s_open || !id || !id[0]) return r;

    int p = present_find(id);
    if (p >= 0) {  // registered already — this tap is ignored
        r.status = ATT_ALREADY_PRESENT;
        r.minutes = s_present_min[p];
        return r;
    }

    int t = tapin_find(id);
    if (t < 0) {  // first tap: record arrival
        tapin_add(id, now_us);
        r.status = ATT_IN_PROGRESS;
        r.remaining = remaining_min(0, threshold_min);
        return r;
    }

    long long elapsed = tapin_elapsed(t, now_us);
    int minutes = (int)(elapsed / MINUTE_US);
    if (minutes < threshold_min) {
        // Too soon: nothing is written and the arrival stands, so the student
        // can tap again later and still be registered.
        r.status = ATT_TOO_EARLY;
        r.minutes = minutes;
        r.remaining = remaining_min(elapsed, threshold_min);
        return r;
    }

    // Threshold met: register the presence and close out the arrival.
    tapin_remove(t);
    present_set(id, true, minutes);
    append_record(id, true, minutes, true);
    r.status = ATT_PRESENT;
    r.minutes = minutes;
    return r;
}

att_state_t attendance_tap_state(const char* id, long long now_us, int threshold_min) {
    att_state_t r = {ATT_ABSENT, 0, 0};
    if (!s_open || !id || !id[0]) return r;
    int t = tapin_find(id);
    if (t >= 0) {
        long long elapsed = tapin_elapsed(t, now_us);
        r.status = ATT_IN_PROGRESS;
        r.minutes = (int)(elapsed / MINUTE_US);
        r.remaining = remaining_min(elapsed, threshold_min);
        return r;
    }
    int p = present_find(id);
    if (p >= 0) {
        r.status = ATT_PRESENT;
        r.minutes = s_present_min[p];
    }
    return r;
}

void attendance_close(void) {
    s_open = false;
    s_dir[0] = '\0';
    s_date[0] = '\0';
    s_present_count = 0;
    s_tapin_count = 0;
}

int attendance_list_dates(const char* class_dir, char dates[][12], int max) {
    char adir[64];
    snprintf(adir, sizeof(adir), "/classes/%s/attendance", class_dir);
    File d = SD_MMC.open(adir);
    if (!d || !d.isDirectory()) {
        if (d) d.close();
        return 0;
    }

    int n = 0;
    File e;
    while (n < max && (e = d.openNextFile())) {
        const char* name = e.name();  // basename on this core (see photo_store)
        size_t len = strlen(name);
        if (len == 16 && strcmp(name + 10, ".jsonl") == 0) {
            snprintf(dates[n], 12, "%.10s", name);
            n++;
        }
        e.close();
    }
    d.close();

    // Newest first; "YYYY-MM-DD" sorts lexically == chronologically.
    for (int i = 1; i < n; i++) {
        char key[12];
        snprintf(key, sizeof(key), "%s", dates[i]);
        int j = i;
        while (j > 0 && strcmp(dates[j - 1], key) < 0) {
            snprintf(dates[j], 12, "%s", dates[j - 1]);
            j--;
        }
        snprintf(dates[j], 12, "%s", key);
    }
    return n;
}

int attendance_present_for(const char* class_dir, const char* date) {
    static char tmp[MAX_PRESENT][ID_LEN];  // read-only fold, avoids big stack
    int count = 0;
    char path[80];
    att_path(class_dir, date, path, sizeof(path));
    fold_file(path, tmp, &count);
    return count;
}

int attendance_clear(const char* class_dir, int* out_failed) {
    attendance_close();  // never leave a session open on files we're deleting
    // List the dates first (static buffer, not stack), then delete — removing
    // files while iterating the directory would be unsafe.
    static char dates[512][12];
    int n = attendance_list_dates(class_dir, dates, 512);
    int removed = 0;
    for (int i = 0; i < n; i++) {
        char path[80];
        att_path(class_dir, dates[i], path, sizeof(path));
        if (SD_MMC.remove(path)) {
            removed++;
        } else {
            // Silently dropping this is how a partial wipe used to look clean.
            ESP_LOGE(TAG, "clear %s: could not delete %s", class_dir, path);
        }
    }
    if (out_failed) *out_failed = n - removed;
    return removed;
}
