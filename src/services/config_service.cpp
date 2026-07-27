#include "services/config_service.h"

#include <ArduinoJson.h>
#include <FS.h>
#include <SD_MMC.h>
#include <ctype.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "app/event_bus.h"
#include "app/uid.h"
#include "esp32-hal-log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "services/roster_service.h"
#include "storage/sd_card.h"

static const char* TAG = "config";

static constexpr const char* CONFIG_PATH = "/config.json";
static constexpr uint32_t RETRY_PERIOD_MS = 3000;

static SemaphoreHandle_t s_lock = nullptr;  // guards s_config + s_error
static config_status_t s_status = CONFIG_NO_SD;
static device_config_t s_config = {};
static char s_error[128] = "";

// Writes the failure reason into the caller's `err` buffer (not shared state)
// and returns the status. Non-mutating, so the import validator can reuse it.
static config_status_t cfg_fail(char* err, size_t cap, config_status_t st, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    if (err && cap) vsnprintf(err, cap, fmt, ap);
    va_end(ap);
    if (err) ESP_LOGE(TAG, "%s", err);
    return st;
}

// Reads, parses, and validates a config.json at `path` into the caller-provided
// `out`, writing any failure reason into `err`. Touches NO shared state
// (s_config / s_error), so both the live loader and config_validate_tree() can
// call it. Reads into a heap buffer (not a 2 KB stack array) because validation
// runs deep inside the import call chain on the LVGL thread, where an extra 2 KB
// on the stack plus a JsonDocument would risk the loopTask stack. Logs the raw
// file contents on the way (the boot-time debug aid). Returns the status.
static config_status_t parse_config_file(const char* path, device_config_t* out, char* err,
                                         size_t errcap) {
    if (out) *out = device_config_t{};

    if (!sd_card_mount()) {
        return cfg_fail(err, errcap, CONFIG_NO_SD,
                        "No SD card, or card not readable (FAT32 required)");
    }
    if (!SD_MMC.exists(path)) {
        return cfg_fail(err, errcap, CONFIG_NO_FILE, "config.json not found on the SD card root");
    }
    File f = SD_MMC.open(path, FILE_READ);
    if (!f) {
        return cfg_fail(err, errcap, CONFIG_NO_FILE, "config.json could not be opened");
    }
    char* buf = (char*)malloc(2048);
    if (!buf) {
        f.close();
        return cfg_fail(err, errcap, CONFIG_BAD_JSON, "Out of memory reading config.json");
    }
    size_t n = f.readBytes(buf, 2047);
    buf[n] = '\0';
    f.close();

    // Boot-time debug: dump exactly what's on the card.
    ESP_LOGI(TAG, "%s (%u bytes):\n%s", path, (unsigned)n, buf);

    JsonDocument doc;
    DeserializationError jerr = deserializeJson(doc, buf);
    if (jerr) {
        config_status_t st = cfg_fail(err, errcap, CONFIG_BAD_JSON, "config.json: %s", jerr.c_str());
        free(buf);
        return st;
    }

    device_config_t parsed = {};
    for (JsonObject t : doc["teachers"].as<JsonArray>()) {
        if (parsed.teacher_count >= CONFIG_MAX_TEACHERS) {
            ESP_LOGW(TAG, "more than %d teachers listed, extras ignored", CONFIG_MAX_TEACHERS);
            break;
        }
        teacher_t* dst = &parsed.teachers[parsed.teacher_count++];
        snprintf(dst->name, sizeof(dst->name), "%s", (const char*)(t["name"] | ""));
        snprintf(dst->email, sizeof(dst->email), "%s", (const char*)(t["email"] | ""));
        snprintf(dst->rfid_uid, sizeof(dst->rfid_uid), "%s", (const char*)(t["rfid_uid"] | ""));
        snprintf(dst->password, sizeof(dst->password), "%s", (const char*)(t["password"] | ""));
    }
    parsed.capture_photos = doc["capture_photos"] | false;
    free(buf);  // parsed holds its own copies now; doc/buf are done with

    if (parsed.teacher_count == 0) {
        return cfg_fail(err, errcap, CONFIG_BAD_JSON, "config.json lists no teachers");
    }

    // Passwords are entered on numeric keypads, so they must be digits-only.
    // Empty passwords (RFID-only professors) are exempt.
    for (int i = 0; i < parsed.teacher_count; i++) {
        const char* pw = parsed.teachers[i].password;
        if (pw[0] == '\0') continue;
        for (const char* c = pw; *c; c++) {
            if (!isdigit((unsigned char)*c)) {
                return cfg_fail(err, errcap, CONFIG_NON_NUMERIC_PASSWORD,
                                "%s has a non-numeric password (digits only)",
                                parsed.teachers[i].name);
            }
        }
    }

    // Passwords must be unique so a typed password identifies one professor.
    // Empty passwords (RFID-only professors) are exempt.
    for (int i = 0; i < parsed.teacher_count; i++) {
        if (parsed.teachers[i].password[0] == '\0') continue;
        for (int j = i + 1; j < parsed.teacher_count; j++) {
            if (strcmp(parsed.teachers[i].password, parsed.teachers[j].password) == 0) {
                return cfg_fail(err, errcap, CONFIG_DUP_PASSWORD,
                                "%s and %s share the same password", parsed.teachers[i].name,
                                parsed.teachers[j].name);
            }
        }
    }

    if (out) *out = parsed;
    ESP_LOGI(TAG, "config parsed: %d teacher(s)", parsed.teacher_count);
    for (int i = 0; i < parsed.teacher_count; i++) {
        ESP_LOGI(TAG, "  teacher[%d] name='%s' email='%s' uid='%s' password=%s", i,
                 parsed.teachers[i].name, parsed.teachers[i].email, parsed.teachers[i].rfid_uid,
                 parsed.teachers[i].password[0] ? "set" : "(none)");
    }
    return CONFIG_OK;
}

// Live loader: parse the on-card /config.json and publish it into shared state.
static config_status_t load_config(void) {
    device_config_t parsed;
    char err[128];
    config_status_t st = parse_config_file(CONFIG_PATH, &parsed, err, sizeof(err));

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (st == CONFIG_OK) {
        s_config = parsed;
        s_error[0] = '\0';
    } else {
        snprintf(s_error, sizeof(s_error), "%s", err);
    }
    xSemaphoreGive(s_lock);
    return st;
}

static void publish(config_status_t st) {
    s_status = st;
    app_event_t ev = {};
    ev.type = APP_EVENT_CONFIG_STATE;
    ev.config.status = (uint8_t)st;
    event_bus_post(&ev);
}

// Retries the load until it succeeds so a card inserted after boot is picked
// up. Publishes only on change to keep the event bus quiet, then exits once
// the config is valid.
static void config_task(void*) {
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(RETRY_PERIOD_MS));
        config_status_t st = load_config();
        if (st != s_status) publish(st);
        if (st == CONFIG_OK) break;
    }
    vTaskDelete(nullptr);
}

bool config_service_start(void) {
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return false;

    publish(load_config());  // first attempt, synchronous, sets initial state

    if (s_status != CONFIG_OK) {
        xTaskCreate(config_task, "config", 5120, nullptr, 2, nullptr);
    }
    return true;
}

void config_service_reload(void) { publish(load_config()); }

config_status_t config_get_status(void) { return s_status; }

void config_get_error(char* out, size_t cap) {
    if (!out || !cap) return;
    out[0] = '\0';
    if (!s_lock) return;  // service never started
    xSemaphoreTake(s_lock, portMAX_DELAY);
    snprintf(out, cap, "%s", s_error);
    xSemaphoreGive(s_lock);
}

void config_get(device_config_t* out) {
    if (!out) return;
    *out = device_config_t{};
    if (!s_lock) return;  // service never started (e.g. roster read before it)
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_status == CONFIG_OK) *out = s_config;
    xSemaphoreGive(s_lock);
}

bool config_find_teacher_by_uid(const char* uid_hex, teacher_t* out) {
    if (!s_lock || !uid_hex) return false;
    char scanned[48];
    uid_normalize(uid_hex, scanned, sizeof(scanned));
    if (scanned[0] == '\0') return false;

    bool found = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_status == CONFIG_OK) {
        for (int i = 0; i < s_config.teacher_count; i++) {
            char configured[48];
            uid_normalize(s_config.teachers[i].rfid_uid, configured, sizeof(configured));
            if (configured[0] && strcmp(scanned, configured) == 0) {
                if (out) *out = s_config.teachers[i];
                found = true;
                break;
            }
        }
    }
    xSemaphoreGive(s_lock);
    return found;
}

bool config_find_teacher_by_password(const char* password, teacher_t* out) {
    if (!s_lock || !password || password[0] == '\0') return false;

    bool found = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_status == CONFIG_OK) {
        for (int i = 0; i < s_config.teacher_count; i++) {
            if (s_config.teachers[i].password[0] == '\0') continue;
            if (strcmp(password, s_config.teachers[i].password) == 0) {
                if (out) *out = s_config.teachers[i];
                found = true;
                break;
            }
        }
    }
    xSemaphoreGive(s_lock);
    return found;
}

bool config_teacher_has_password(const char* email, const char* rfid_uid) {
    if (!s_lock) return false;

    bool has = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_status == CONFIG_OK) {
        for (int i = 0; i < s_config.teacher_count; i++) {
            const teacher_t* c = &s_config.teachers[i];
            bool match = (email && email[0])
                             ? (strcmp(c->email, email) == 0)
                             : (rfid_uid && rfid_uid[0] && strcmp(c->rfid_uid, rfid_uid) == 0);
            if (match) {
                has = c->password[0] != '\0';
                break;
            }
        }
    }
    xSemaphoreGive(s_lock);
    return has;
}

// Serializes doc to a temp file then renames over /config.json, so a power cut
// leaves either the old file or the new one — never a half-written one. Mirrors
// roster_service's write_json_file (kept local: services own their own files).
static bool write_config_file(JsonDocument& doc) {
    size_t need = measureJsonPretty(doc);
    char* buf = (char*)malloc(need + 1);
    if (!buf) return false;
    size_t len = serializeJsonPretty(doc, buf, need + 1);

    const char* tmp = "/config.json.tmp";
    File f = SD_MMC.open(tmp, FILE_WRITE, true);
    if (!f) {
        free(buf);
        return false;
    }
    size_t written = f.write((const uint8_t*)buf, len);
    f.close();
    free(buf);
    if (written != len) return false;

    SD_MMC.remove(CONFIG_PATH);  // FAT rename requires the target to be absent
    return SD_MMC.rename(tmp, CONFIG_PATH);
}

static config_result_t result(bool ok, const char* fmt, ...) {
    config_result_t r = {ok, ""};
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(r.message, sizeof(r.message), fmt, ap);
    va_end(ap);
    return r;
}

config_result_t config_set_password(const char* email, const char* rfid_uid,
                                    const char* new_password) {
    if (!new_password || new_password[0] == '\0') {
        return result(false, "Enter a password");
    }
    for (const char* c = new_password; *c; c++) {
        if (!isdigit((unsigned char)*c)) return result(false, "Digits only");
    }
    if (strlen(new_password) >= sizeof(((teacher_t*)0)->password)) {
        return result(false, "Password too long");
    }
    if (config_get_status() != CONFIG_OK) {
        return result(false, "Fix config.json first");
    }

    // Re-read the file so we preserve any fields we don't model. The read
    // buffer is heap-allocated (not a 2 KB stack array): this runs deep inside
    // an LVGL event callback and then calls load_config(), whose own 2 KB
    // buffer would otherwise stack on top of it and overflow loopTask.
    File f = SD_MMC.open(CONFIG_PATH, FILE_READ);
    if (!f) return result(false, "Could not open config.json");
    char* buf = (char*)malloc(2048);
    if (!buf) {
        f.close();
        return result(false, "Out of memory");
    }
    size_t n = f.readBytes(buf, 2047);
    buf[n] = '\0';
    f.close();

    // deserializeJson runs in zero-copy mode on a mutable char*, so `doc` holds
    // pointers into buf — keep buf alive until after write_config_file().
    JsonDocument doc;
    if (deserializeJson(doc, buf)) {
        free(buf);
        return result(false, "config.json unreadable");
    }

    JsonArray teachers = doc["teachers"].as<JsonArray>();
    JsonObject me;
    for (JsonObject t : teachers) {
        const char* e = t["email"] | "";
        const char* u = t["rfid_uid"] | "";
        bool match = (email && email[0]) ? (strcmp(e, email) == 0)
                                         : (rfid_uid && rfid_uid[0] && strcmp(u, rfid_uid) == 0);
        if (match) {
            me = t;
            continue;
        }
        // Uniqueness: no other professor may already hold this password.
        const char* pw = t["password"] | "";
        if (pw[0] && strcmp(pw, new_password) == 0) {
            free(buf);
            return result(false, "Already used by another professor");
        }
    }
    if (me.isNull()) {
        free(buf);
        return result(false, "Your account is not in config.json");
    }

    me["password"] = new_password;
    bool wrote = write_config_file(doc);
    free(buf);  // done with doc; free before load_config()'s own read buffer
    if (!wrote) return result(false, "Could not save to SD card");

    publish(load_config());  // reflect the new state everywhere
    return result(true, "Password saved");
}

config_result_t config_set_rfid(const char* email, const char* rfid_uid, const char* new_uid) {
    if (!new_uid || new_uid[0] == '\0') {
        return result(false, "Tap a card first");
    }
    if (strlen(new_uid) >= sizeof(((teacher_t*)0)->rfid_uid)) {
        return result(false, "Card ID too long");
    }
    if (config_get_status() != CONFIG_OK) {
        return result(false, "Fix config.json first");
    }

    char new_norm[48];
    uid_normalize(new_uid, new_norm, sizeof(new_norm));
    if (new_norm[0] == '\0') {
        return result(false, "Card ID has no usable characters");
    }

    // A professor must not claim a card that is already a student's, or tapping
    // it would both unlock the device and register attendance. Mirrors the
    // professor-collision guard roster_enroll_* applies in the other direction.
    char student_name[48];
    if (roster_uid_belongs_to_student(new_uid, student_name, sizeof(student_name))) {
        return result(false, "Card belongs to student %s", student_name);
    }

    // Re-read the file so we preserve any fields we don't model. Heap buffer
    // (not a 2 KB stack array) for the same reason as config_set_password: this
    // runs inside an LVGL callback and load_config() adds another 2 KB below.
    File f = SD_MMC.open(CONFIG_PATH, FILE_READ);
    if (!f) return result(false, "Could not open config.json");
    char* buf = (char*)malloc(2048);
    if (!buf) {
        f.close();
        return result(false, "Out of memory");
    }
    size_t n = f.readBytes(buf, 2047);
    buf[n] = '\0';
    f.close();

    JsonDocument doc;
    if (deserializeJson(doc, buf)) {
        free(buf);
        return result(false, "config.json unreadable");
    }

    JsonArray teachers = doc["teachers"].as<JsonArray>();
    JsonObject me;
    for (JsonObject t : teachers) {
        const char* e = t["email"] | "";
        const char* u = t["rfid_uid"] | "";
        bool match = (email && email[0]) ? (strcmp(e, email) == 0)
                                         : (rfid_uid && rfid_uid[0] && strcmp(u, rfid_uid) == 0);
        if (match) {
            me = t;
            continue;
        }
        // Uniqueness: no other professor may already carry this card.
        char other_norm[48];
        uid_normalize(u, other_norm, sizeof(other_norm));
        if (other_norm[0] && strcmp(other_norm, new_norm) == 0) {
            free(buf);
            return result(false, "Already bound to another professor");
        }
    }
    if (me.isNull()) {
        free(buf);
        return result(false, "Your account is not in config.json");
    }

    me["rfid_uid"] = new_uid;
    bool wrote = write_config_file(doc);
    free(buf);  // done with doc; free before load_config()'s own read buffer
    if (!wrote) return result(false, "Could not save to SD card");

    publish(load_config());  // reflect the new state everywhere
    return result(true, "Card updated");
}

bool config_validate_tree(const char* root, char* msg, size_t cap) {
    char path[96];
    snprintf(path, sizeof(path), "%s/config.json", root ? root : "");
    device_config_t scratch;
    char err[128];
    config_status_t st = parse_config_file(path, &scratch, err, sizeof(err));
    if (msg && cap) snprintf(msg, cap, "%s", st == CONFIG_OK ? "" : err);
    return st == CONFIG_OK;
}

bool config_photo_capture_enabled(void) {
    if (!s_lock) return false;
    bool en = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_status == CONFIG_OK) en = s_config.capture_photos;
    xSemaphoreGive(s_lock);
    return en;
}

config_result_t config_set_photo_capture(bool enabled) {
    if (config_get_status() != CONFIG_OK) return result(false, "Fix config.json first");

    File f = SD_MMC.open(CONFIG_PATH, FILE_READ);
    if (!f) return result(false, "Could not open config.json");
    char* buf = (char*)malloc(2048);
    if (!buf) {
        f.close();
        return result(false, "Out of memory");
    }
    size_t n = f.readBytes(buf, 2047);
    buf[n] = '\0';
    f.close();

    JsonDocument doc;
    if (deserializeJson(doc, buf)) {
        free(buf);
        return result(false, "config.json unreadable");
    }
    doc["capture_photos"] = enabled;
    bool wrote = write_config_file(doc);
    free(buf);
    if (!wrote) return result(false, "Could not save to SD card");

    publish(load_config());
    return result(true, enabled ? "Photo capture enabled" : "Photo capture disabled");
}

bool config_has_any_password(void) {
    if (!s_lock) return false;
    bool any = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_status == CONFIG_OK) {
        for (int i = 0; i < s_config.teacher_count; i++) {
            if (s_config.teachers[i].password[0]) {
                any = true;
                break;
            }
        }
    }
    xSemaphoreGive(s_lock);
    return any;
}
