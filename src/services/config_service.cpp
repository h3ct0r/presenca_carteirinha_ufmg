#include "services/config_service.h"

#include <ArduinoJson.h>
#include <FS.h>
#include <SD_MMC.h>
#include <ctype.h>
#include <stdarg.h>
#include <string.h>

#include "app/event_bus.h"
#include "app/uid.h"
#include "esp32-hal-log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "storage/sd_card.h"

static const char* TAG = "config";

static constexpr const char* CONFIG_PATH = "/config.json";
static constexpr uint32_t RETRY_PERIOD_MS = 3000;

static SemaphoreHandle_t s_lock = nullptr;  // guards s_config + s_error
static config_status_t s_status = CONFIG_NO_SD;
static device_config_t s_config = {};
static char s_error[128] = "";

// Records the failure reason (shown on the idle screen) and returns status.
static config_status_t fail(config_status_t st, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    vsnprintf(s_error, sizeof(s_error), fmt, ap);
    xSemaphoreGive(s_lock);
    va_end(ap);
    ESP_LOGE(TAG, "%s", s_error);
    return st;
}

// Reads and parses /config.json. Logs the raw file contents on the way (the
// requested boot-time debug aid). Does not touch shared state unless it
// succeeds. Returns the resulting status.
static config_status_t load_config(void) {
    if (!sd_card_mount()) {
        return fail(CONFIG_NO_SD, "No SD card, or card not readable (FAT32 required)");
    }

    if (!SD_MMC.exists(CONFIG_PATH)) {
        return fail(CONFIG_NO_FILE, "config.json not found on the SD card root");
    }

    File f = SD_MMC.open(CONFIG_PATH, FILE_READ);
    if (!f) {
        return fail(CONFIG_NO_FILE, "config.json could not be opened");
    }

    char buf[2048];
    size_t n = f.readBytes(buf, sizeof(buf) - 1);
    buf[n] = '\0';
    f.close();

    // Boot-time debug: dump exactly what's on the card.
    ESP_LOGI(TAG, "%s (%u bytes):\n%s", CONFIG_PATH, (unsigned)n, buf);

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, buf);
    if (err) {
        return fail(CONFIG_BAD_JSON, "config.json: %s", err.c_str());
    }

    device_config_t parsed = {};
    for (JsonObject t : doc["teachers"].as<JsonArray>()) {
        if (parsed.teacher_count >= CONFIG_MAX_TEACHERS) {
            ESP_LOGW(TAG, "more than %d teachers listed, extras ignored", CONFIG_MAX_TEACHERS);
            break;
        }
        const char* name = t["name"] | "";
        const char* email = t["email"] | "";
        const char* uid = t["rfid_uid"] | "";
        const char* pw = t["password"] | "";
        teacher_t* dst = &parsed.teachers[parsed.teacher_count++];
        snprintf(dst->name, sizeof(dst->name), "%s", name);
        snprintf(dst->email, sizeof(dst->email), "%s", email);
        snprintf(dst->rfid_uid, sizeof(dst->rfid_uid), "%s", uid);
        snprintf(dst->password, sizeof(dst->password), "%s", pw);
    }

    parsed.capture_photos = doc["capture_photos"] | false;

    if (parsed.teacher_count == 0) {
        return fail(CONFIG_BAD_JSON, "config.json lists no teachers");
    }

    // Passwords are entered on numeric keypads, so they must be digits-only.
    // Empty passwords (RFID-only professors) are exempt.
    for (int i = 0; i < parsed.teacher_count; i++) {
        const char* pw = parsed.teachers[i].password;
        if (pw[0] == '\0') continue;
        for (const char* c = pw; *c; c++) {
            if (!isdigit((unsigned char)*c)) {
                return fail(CONFIG_NON_NUMERIC_PASSWORD,
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
                return fail(CONFIG_DUP_PASSWORD, "%s and %s share the same password",
                            parsed.teachers[i].name, parsed.teachers[j].name);
            }
        }
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_config = parsed;
    s_error[0] = '\0';
    xSemaphoreGive(s_lock);

    ESP_LOGI(TAG, "config loaded: %d teacher(s)", parsed.teacher_count);
    for (int i = 0; i < parsed.teacher_count; i++) {
        ESP_LOGI(TAG, "  teacher[%d] name='%s' email='%s' uid='%s' password=%s", i,
                 parsed.teachers[i].name, parsed.teachers[i].email, parsed.teachers[i].rfid_uid,
                 parsed.teachers[i].password[0] ? "set" : "(none)");
    }
    return CONFIG_OK;
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
