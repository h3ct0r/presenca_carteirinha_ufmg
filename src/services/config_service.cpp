#include "services/config_service.h"

#include <ArduinoJson.h>
#include <FS.h>
#include <SD_MMC.h>
#include <ctype.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "app/credential.h"
#include "app/event_bus.h"
#include "app/uid.h"
#include "esp32-hal-log.h"
#include "services/device_secret.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "services/roster_service.h"
#include "storage/atomic_file.h"
#include "storage/sd_card.h"

static const char* TAG = "config";

static constexpr const char* CONFIG_PATH = "/config.json";
static constexpr uint32_t RETRY_PERIOD_MS = 3000;
// The deep path here is a *failed* load: parse_config_file's ~1.7 kB frame plus
// newlib's ~1.1 kB vfprintf frame for the ESP_LOG that reports the failure.
// config_task logs its real headroom once, so shrinking this is not guesswork.
static constexpr uint32_t TASK_STACK_BYTES = 5120;

// Read buffer for config.json, heap-allocated at every use site.
//
// Sized for the CONVERTED file, which is much larger than an authored one: a
// stored password fingerprint is 67 characters against a 6-digit password, so
// eight professors add ~600 bytes on conversion alone. Pretty-printed, eight
// teachers land near 2.1 kB — the previous 2048 would have silently truncated
// the read, failed the parse, and locked the device out of its own config.
static constexpr size_t CONFIG_READ_BYTES = 4096;

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
// on the stack plus a JsonDocument would risk the loopTask stack. Returns the
// status.
static config_status_t parse_config_file(const char* path, device_config_t* out, char* err,
                                         size_t errcap) {
    if (out) *out = device_config_t{};

    if (!sd_card_mount()) {
        return cfg_fail(err, errcap, CONFIG_NO_SD,
                        "No SD card, or card not readable (FAT32 required)");
    }
    // A password/card write interrupted by a power cut leaves the config at
    // <path>.bak. Restore it before concluding the device has no configuration —
    // that conclusion offers to overwrite it with a fresh first-run setup.
    atomic_file_recover(path);
    if (!SD_MMC.exists(path)) {
        return cfg_fail(err, errcap, CONFIG_NO_FILE, "config.json not found on the SD card root");
    }
    File f = SD_MMC.open(path, FILE_READ);
    if (!f) {
        return cfg_fail(err, errcap, CONFIG_NO_FILE, "config.json could not be opened");
    }
    char* buf = (char*)malloc(CONFIG_READ_BYTES);
    if (!buf) {
        f.close();
        return cfg_fail(err, errcap, CONFIG_BAD_JSON, "Out of memory reading config.json");
    }
    size_t n = f.readBytes(buf, CONFIG_READ_BYTES - 1);
    buf[n] = '\0';
    f.close();

    // Deliberately NOT dumping `buf`: config.json carries every professor's
    // password in cleartext, and this runs at boot and on every 3 s retry, so a
    // raw dump puts them on the serial console for anyone with the USB port.
    // The per-teacher lines below report the same structure with the password
    // reduced to "set"/"(none)", which is what the debugging actually needs.
    ESP_LOGI(TAG, "%s: %u bytes read", path, (unsigned)n);

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
    free(buf);  // parsed holds its own copies now; doc/buf are done with

    if (parsed.teacher_count == 0) {
        return cfg_fail(err, errcap, CONFIG_BAD_JSON, "config.json lists no teachers");
    }

    // Passwords are entered on numeric keypads, so they must be digits-only.
    // Empty passwords (RFID-only professors) are exempt, and so is an already
    // converted one — a stored fingerprint is hex, and checking it for digits
    // would reject every device that has done its conversion. This still catches
    // a bad AUTHORED password, which is the only form the rule can apply to and
    // the form config_validate_tree() sees when vetting an import.
    for (int i = 0; i < parsed.teacher_count; i++) {
        const char* pw = parsed.teachers[i].password;
        if (pw[0] == '\0') continue;
        if (credential_is_fingerprint(pw, PASSWORD_HASH_HEX)) continue;
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
        // Never the uid or the password themselves — before conversion they are
        // still cleartext, and this runs at boot and on every 3 s retry.
        const teacher_t* t = &parsed.teachers[i];
        ESP_LOGI(TAG, "  teacher[%d] name='%s' email='%s' card=%s password=%s", i, t->name,
                 t->email,
                 !t->rfid_uid[0] ? "(none)"
                 : credential_is_fingerprint(t->rfid_uid, UID_FINGERPRINT_HEX) ? "bound"
                                                                               : "PLAINTEXT",
                 !t->password[0] ? "(none)"
                 : credential_is_fingerprint(t->password, PASSWORD_HASH_HEX) ? "set"
                                                                             : "PLAINTEXT");
    }
    return CONFIG_OK;
}

// --- authoring plaintext -> stored fingerprints ------------------------------
//
// The config-builder writes the card uid and the password in the clear: it runs
// on a laptop and cannot know this device's key. The device converts them once,
// on the first load that sees them, and rewrites config.json. After that the
// card carries only fingerprints and nothing here ever reads plaintext again.
//
// Converting (rather than rejecting) is what keeps an IMPORTED professor card
// working — rejecting would silently unbind every card in every imported config.

// Does any field still hold the authoring form?
static bool needs_conversion(const device_config_t* c) {
    for (int i = 0; i < c->teacher_count; i++) {
        const teacher_t* t = &c->teachers[i];
        if (t->password[0] && !credential_is_fingerprint(t->password, PASSWORD_HASH_HEX)) {
            return true;
        }
        if (t->rfid_uid[0] && !credential_is_fingerprint(t->rfid_uid, UID_FINGERPRINT_HEX)) {
            return true;
        }
    }
    return false;
}

// Rewrites config.json with every plaintext password/uid replaced. A DOM edit,
// so fields we do not model survive. Returns CONFIG_OK, or the reason it could
// not — never a partial write (atomic_file_write keeps the original recoverable).
static config_status_t convert_config_file(char* err, size_t errcap) {
    uint8_t key[DEVICE_SECRET_LEN];
    if (!device_secret_get(key)) {
        return cfg_fail(err, errcap, CONFIG_NO_KEY,
                        "Device key unavailable - cannot secure config.json");
    }

    File f = SD_MMC.open(CONFIG_PATH, FILE_READ);
    if (!f) return cfg_fail(err, errcap, CONFIG_BAD_JSON, "Could not reopen config.json");
    char* buf = (char*)malloc(CONFIG_READ_BYTES);
    if (!buf) {
        f.close();
        return cfg_fail(err, errcap, CONFIG_BAD_JSON, "Out of memory securing config.json");
    }
    size_t n = f.readBytes(buf, CONFIG_READ_BYTES - 1);
    buf[n] = '\0';
    f.close();

    JsonDocument doc;
    if (deserializeJson(doc, buf)) {
        free(buf);
        return cfg_fail(err, errcap, CONFIG_BAD_JSON, "config.json unreadable");
    }

    int converted = 0;
    for (JsonObject t : doc["teachers"].as<JsonArray>()) {
        const char* pw = t["password"] | "";
        if (pw[0] && !credential_is_fingerprint(pw, PASSWORD_HASH_HEX)) {
            // The digits-only and length rules apply to the plaintext, and this
            // is the last moment it exists — after hashing there is nothing left
            // to check. parse_config_file already rejected non-digits, so this
            // only has to guard the length.
            if (strlen(pw) > (size_t)CONFIG_MAX_PASSWORD_PLAINTEXT) {
                free(buf);
                return cfg_fail(err, errcap, CONFIG_BAD_JSON,
                                "A password is longer than %d characters",
                                CONFIG_MAX_PASSWORD_PLAINTEXT);
            }
            char hashed[PASSWORD_HASH_CAP];
            password_hash(key, sizeof(key), pw, hashed, sizeof(hashed));
            if (!hashed[0]) {
                free(buf);
                return cfg_fail(err, errcap, CONFIG_NO_KEY, "Could not secure a password");
            }
            t["password"] = hashed;
            converted++;
        }
        const char* uid = t["rfid_uid"] | "";
        if (uid[0] && !credential_is_fingerprint(uid, UID_FINGERPRINT_HEX)) {
            char fp[UID_FINGERPRINT_CAP];
            uid_fingerprint(key, sizeof(key), uid, fp, sizeof(fp));
            if (!fp[0]) {
                // Only reachable for a uid with no usable characters at all;
                // treat it as unbound rather than storing a broken value.
                t["rfid_uid"] = "";
            } else {
                t["rfid_uid"] = fp;
            }
            converted++;
        }
    }

    // ArduinoJson keeps pointers into `buf` for strings it did not copy, so the
    // buffer has to outlive the serialize below.
    size_t need = measureJsonPretty(doc);
    char* out = (char*)malloc(need + 1);
    if (!out) {
        free(buf);
        return cfg_fail(err, errcap, CONFIG_BAD_JSON, "Out of memory securing config.json");
    }
    size_t len = serializeJsonPretty(doc, out, need + 1);
    bool wrote = atomic_file_write(CONFIG_PATH, (const uint8_t*)out, len, "config.json");
    free(out);
    free(buf);
    memset(key, 0, sizeof(key));

    if (!wrote) {
        // Loud, and it must not read as success: the values on the card are still
        // plaintext, so every comparison below would reject every card and
        // password until this succeeds.
        return cfg_fail(err, errcap, CONFIG_BAD_JSON,
                        "Could not secure config.json - SD full or write-protected?");
    }
    ESP_LOGW(TAG, "converted %d authored credential(s) to fingerprints", converted);
    return CONFIG_OK;
}

// Live loader: parse the on-card /config.json and publish it into shared state.
//
// `parsed` is heap-allocated, not a local: a device_config_t is ~1.5 kB and
// parse_config_file() keeps one of its own, so two on the stack plus newlib's
// 1.1 kB vfprintf frame (any ESP_LOG on the failure path) overflowed the 5 kB
// "config" task — a stack-protection panic on every boot with no config.json.
// Same reasoning as the read buffer inside parse_config_file().
static config_status_t load_config(void) {
    device_config_t* parsed = (device_config_t*)malloc(sizeof(device_config_t));
    char err[128];
    if (!parsed) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        snprintf(s_error, sizeof(s_error), "Out of memory reading config.json");
        xSemaphoreGive(s_lock);
        return CONFIG_BAD_JSON;
    }
    config_status_t st = parse_config_file(CONFIG_PATH, parsed, err, sizeof(err));

    // Authored plaintext gets converted once, here in the LIVE path only — never
    // in parse_config_file(), which config_validate_tree() also drives against a
    // staged tree it must not touch. The re-parse afterwards is what publishes
    // the fingerprints, and is also the proof the rewrite is still readable.
    if (st == CONFIG_OK && needs_conversion(parsed)) {
        st = convert_config_file(err, sizeof(err));
        if (st == CONFIG_OK) st = parse_config_file(CONFIG_PATH, parsed, err, sizeof(err));
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (st == CONFIG_OK) {
        s_config = *parsed;
        s_error[0] = '\0';
    } else {
        snprintf(s_error, sizeof(s_error), "%s", err);
    }
    xSemaphoreGive(s_lock);
    free(parsed);
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
    bool logged_stack = false;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(RETRY_PERIOD_MS));
        config_status_t st = load_config();
        // Once, after a full load has run: this task's headroom used to be
        // negative on the no-config path (a stack-protection panic, not a
        // message), so report the real margin instead of leaving it invisible
        // until it overflows again. The failure path is the deep one — it stacks
        // newlib's ~1.1 kB vfprintf frame on top of the parse.
        if (!logged_stack) {
            logged_stack = true;
            ESP_LOGI(TAG, "config task stack: %u B unused of %u",
                     (unsigned)(uxTaskGetStackHighWaterMark(nullptr) * sizeof(StackType_t)),
                     (unsigned)TASK_STACK_BYTES);
        }
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
        xTaskCreate(config_task, "config", TASK_STACK_BYTES, nullptr, 2, nullptr);
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
    // Fingerprint the SCANNED card once, then compare against what is stored —
    // which is already a fingerprint, and already canonical, so no normalizing
    // on that side. uid_fingerprint() normalizes internally, so the separators
    // and case the reader happens to produce do not matter.
    uint8_t key[DEVICE_SECRET_LEN];
    if (!device_secret_get(key)) return false;  // fail closed
    char scanned[UID_FINGERPRINT_CAP];
    uid_fingerprint(key, sizeof(key), uid_hex, scanned, sizeof(scanned));
    memset(key, 0, sizeof(key));
    if (scanned[0] == '\0') return false;

    bool found = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_status == CONFIG_OK) {
        for (int i = 0; i < s_config.teacher_count; i++) {
            const char* stored = s_config.teachers[i].rfid_uid;
            if (stored[0] && strcmp(scanned, stored) == 0) {
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

    // One derivation per attempt, then plain compares — the single device key
    // (rather than a per-teacher salt) is what keeps this at one hash even with
    // CONFIG_MAX_TEACHERS accounts, and is also what lets parse_config_file
    // still detect two professors sharing a password.
    uint8_t key[DEVICE_SECRET_LEN];
    if (!device_secret_get(key)) return false;  // fail closed
    char typed[PASSWORD_HASH_CAP];
    password_hash(key, sizeof(key), password, typed, sizeof(typed));
    memset(key, 0, sizeof(key));
    if (typed[0] == '\0') return false;

    bool found = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_status == CONFIG_OK) {
        for (int i = 0; i < s_config.teacher_count; i++) {
            if (s_config.teachers[i].password[0] == '\0') continue;
            if (strcmp(typed, s_config.teachers[i].password) == 0) {
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

// Replaces /config.json with doc, crash-safely (storage/atomic_file.h). The old
// remove-then-rename here had a window in which config.json did not exist at
// all — and without it the device cannot be unlocked by anyone.
static bool write_config_file(JsonDocument& doc) {
    size_t need = measureJsonPretty(doc);
    char* buf = (char*)malloc(need + 1);
    if (!buf) return false;
    size_t len = serializeJsonPretty(doc, buf, need + 1);
    bool ok = atomic_file_write(CONFIG_PATH, (const uint8_t*)buf, len, "config.json");
    free(buf);
    return ok;
}

static config_result_t result(bool ok, const char* fmt, ...) {
    config_result_t r = {ok, ""};
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(r.message, sizeof(r.message), fmt, ap);
    va_end(ap);
    return r;
}

// Shared by the bootstrap and the password editor so both enforce the same
// rules with the same wording. Returns an empty message when the password is
// acceptable.
//
// The length floor is enforced on WRITE only, never on parse: raising it must
// not lock out a professor whose shorter password is already in config.json, and
// an imported config.tar is the config-builder's business to validate. It exists
// because the password is the whole unlock credential and the keypad is numeric
// — a 4-digit PIN is 10^4 candidates, which only the login throttle in scr_idle
// makes expensive.
static config_result_t check_password(const char* pw) {
    if (!pw || pw[0] == '\0') return result(false, "Enter a password");
    for (const char* c = pw; *c; c++) {
        if (!isdigit((unsigned char)*c)) return result(false, "Digits only");
    }
    if (strlen(pw) < CONFIG_MIN_PASSWORD_DIGITS) {
        return result(false, "Use at least %d digits", CONFIG_MIN_PASSWORD_DIGITS);
    }
    // NOT sizeof(teacher_t::password): that buffer holds the stored fingerprint
    // now and is far wider than any password a human types, so bounding by it
    // would stop rejecting over-long input entirely.
    if (strlen(pw) > (size_t)CONFIG_MAX_PASSWORD_PLAINTEXT) {
        return result(false, "Password too long");
    }
    return result(true, "");
}

// Derives the stored form of a password/uid for the write paths below. Returns
// false (with `r` set) when the device key is unavailable — the caller must not
// fall back to storing plaintext.
static bool derive_password(const char* plain, char* out, size_t cap, config_result_t* r) {
    uint8_t key[DEVICE_SECRET_LEN];
    if (!device_secret_get(key)) {
        *r = result(false, "Device key unavailable");
        return false;
    }
    password_hash(key, sizeof(key), plain, out, cap);
    memset(key, 0, sizeof(key));
    if (!out[0]) {
        *r = result(false, "Could not secure the password");
        return false;
    }
    return true;
}

static bool derive_uid(const char* plain, char* out, size_t cap, config_result_t* r) {
    uint8_t key[DEVICE_SECRET_LEN];
    if (!device_secret_get(key)) {
        *r = result(false, "Device key unavailable");
        return false;
    }
    uid_fingerprint(key, sizeof(key), plain, out, cap);
    memset(key, 0, sizeof(key));
    if (!out[0]) {
        *r = result(false, "Card ID has no usable characters");
        return false;
    }
    return true;
}

config_result_t config_create_first_teacher(const char* name, const char* password) {
    // The guard, not the caller, is what makes this safe: CONFIG_NO_FILE is the
    // only state where there is nothing to lose. CONFIG_OK would be overwritten,
    // a parse failure would be papered over instead of fixed, and CONFIG_NO_SD
    // has nowhere to write.
    if (config_get_status() != CONFIG_NO_FILE) {
        return result(false, "This device already has a configuration");
    }
    if (!name || name[0] == '\0') return result(false, "Enter a name");
    if (strlen(name) >= sizeof(((teacher_t*)0)->name)) return result(false, "Name too long");
    config_result_t pw = check_password(password);
    if (!pw.ok) return pw;

    // Hashed before it is ever written: the bootstrap must not create the one
    // plaintext password on the card that everything else is removing.
    char hashed[PASSWORD_HASH_CAP];
    config_result_t derr;
    if (!derive_password(password, hashed, sizeof(hashed), &derr)) return derr;

    JsonDocument doc;
    JsonObject t = doc["teachers"].add<JsonObject>();
    t["name"] = name;
    t["email"] = "";
    t["rfid_uid"] = "";
    t["password"] = hashed;

    if (!write_config_file(doc)) return result(false, "Could not save to SD card");

    config_status_t st = load_config();
    publish(st);
    if (st != CONFIG_OK) return result(false, "Saved, but the config did not load");
    return result(true, "Device set up - unlock with your password");
}

config_result_t config_set_password(const char* email, const char* rfid_uid,
                                    const char* new_password) {
    config_result_t pw = check_password(new_password);
    if (!pw.ok) return pw;
    if (config_get_status() != CONFIG_OK) {
        return result(false, "Fix config.json first");
    }

    // Derive once, up front: it is what gets written AND what the uniqueness
    // scan below compares against, since the file holds fingerprints.
    char hashed[PASSWORD_HASH_CAP];
    config_result_t derr;
    if (!derive_password(new_password, hashed, sizeof(hashed), &derr)) return derr;

    // Re-read the file so we preserve any fields we don't model. The read
    // buffer is heap-allocated (not a 2 KB stack array): this runs deep inside
    // an LVGL event callback and then calls load_config(), whose own 2 KB
    // buffer would otherwise stack on top of it and overflow loopTask.
    atomic_file_recover(CONFIG_PATH);
    File f = SD_MMC.open(CONFIG_PATH, FILE_READ);
    if (!f) return result(false, "Could not open config.json");
    char* buf = (char*)malloc(CONFIG_READ_BYTES);
    if (!buf) {
        f.close();
        return result(false, "Out of memory");
    }
    size_t n = f.readBytes(buf, CONFIG_READ_BYTES - 1);
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
        // Uniqueness: no other professor may already hold this password. Still
        // detectable through the fingerprints because every account is keyed by
        // the same device secret — the same password yields the same hash.
        const char* stored = t["password"] | "";
        if (stored[0] && strcmp(stored, hashed) == 0) {
            free(buf);
            return result(false, "Already used by another professor");
        }
    }
    if (me.isNull()) {
        free(buf);
        return result(false, "Your account is not in config.json");
    }

    me["password"] = hashed;
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

    // The stored form. Both the uniqueness scan and the write use it; the raw
    // uid never reaches the card.
    char new_fp[UID_FINGERPRINT_CAP];
    config_result_t derr;
    if (!derive_uid(new_uid, new_fp, sizeof(new_fp), &derr)) return derr;

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
    atomic_file_recover(CONFIG_PATH);
    File f = SD_MMC.open(CONFIG_PATH, FILE_READ);
    if (!f) return result(false, "Could not open config.json");
    char* buf = (char*)malloc(CONFIG_READ_BYTES);
    if (!buf) {
        f.close();
        return result(false, "Out of memory");
    }
    size_t n = f.readBytes(buf, CONFIG_READ_BYTES - 1);
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
        // Uniqueness: no other professor may already carry this card. Both sides
        // are fingerprints under the same key, so equality still means "same
        // physical card" without either being readable.
        if (u[0] && strcmp(u, new_fp) == 0) {
            free(buf);
            return result(false, "Already bound to another professor");
        }
    }
    if (me.isNull()) {
        free(buf);
        return result(false, "Your account is not in config.json");
    }

    me["rfid_uid"] = new_fp;
    bool wrote = write_config_file(doc);
    free(buf);  // done with doc; free before load_config()'s own read buffer
    if (!wrote) return result(false, "Could not save to SD card");

    publish(load_config());  // reflect the new state everywhere
    return result(true, "Card updated");
}

bool config_validate_tree(const char* root, char* msg, size_t cap) {
    char path[96];
    snprintf(path, sizeof(path), "%s/config.json", root ? root : "");
    // Heap, not stack, for the same reason load_config() does it: a
    // device_config_t is ~1.5 kB and parse_config_file() keeps one of its own,
    // and this runs deep inside the import chain on the LVGL thread.
    device_config_t* scratch = (device_config_t*)malloc(sizeof(device_config_t));
    if (!scratch) {
        if (msg && cap) snprintf(msg, cap, "Out of memory reading config.json");
        return false;
    }
    char err[128];
    config_status_t st = parse_config_file(path, scratch, err, sizeof(err));
    free(scratch);
    if (msg && cap) snprintf(msg, cap, "%s", st == CONFIG_OK ? "" : err);
    return st == CONFIG_OK;
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
