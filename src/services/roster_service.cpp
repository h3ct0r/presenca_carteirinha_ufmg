#include "services/roster_service.h"

#include <ArduinoJson.h>
#include <FS.h>
#include <SD_MMC.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app/event_bus.h"
#include "app/uid.h"
#include "esp32-hal-log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "services/config_service.h"
#include "storage/sd_card.h"

static const char* TAG = "roster";

static constexpr const char* STUDENTS_PATH = "/students/students.json";
static constexpr const char* CLASSES_DIR = "/classes";
static constexpr uint32_t RETRY_PERIOD_MS = 3000;
// Sanity caps so a wrong file (or one written by accident) can't exhaust RAM.
static constexpr size_t STUDENTS_MAX_BYTES = 200 * 1024;
static constexpr size_t CLASS_MAX_BYTES = 32 * 1024;

static SemaphoreHandle_t s_lock = nullptr;  // guards data + error string
static roster_status_t s_status = ROSTER_NO_SD;
static char s_error[128] = "";

static student_t s_students[ROSTER_MAX_STUDENTS];
static int s_student_count = 0;
static class_rec_t s_classes[ROSTER_MAX_CLASSES];
static int s_class_count = 0;

// Records the failure reason (shown verbatim on the idle screen, so the text
// must name the file and the problem) and passes the status through.
static roster_status_t fail(roster_status_t st, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_error, sizeof(s_error), fmt, ap);
    va_end(ap);
    ESP_LOGE(TAG, "%s", s_error);
    return st;
}

static int find_student(const char* id) {
    for (int i = 0; i < s_student_count; i++) {
        if (strcmp(s_students[i].id, id) == 0) return i;
    }
    return -1;
}

static roster_status_t load_students(const char* root) {
    s_student_count = 0;

    char path[64];
    snprintf(path, sizeof(path), "%s%s", root, STUDENTS_PATH);
    if (!SD_MMC.exists(path)) {
        return fail(ROSTER_NO_STUDENTS_FILE, "Missing %s", path);
    }
    File f = SD_MMC.open(path, FILE_READ);
    if (!f) return fail(ROSTER_BAD_STUDENTS, "students.json: cannot open file");
    if (f.size() > STUDENTS_MAX_BYTES) {
        f.close();
        return fail(ROSTER_BAD_STUDENTS, "students.json: file too large (%u KB)",
                    (unsigned)(f.size() / 1024));
    }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, f);
    f.close();
    if (err) return fail(ROSTER_BAD_STUDENTS, "students.json: %s", err.c_str());

    JsonArray arr = doc["students"].as<JsonArray>();
    if (arr.isNull()) {
        return fail(ROSTER_BAD_STUDENTS, "students.json: missing \"students\" array");
    }

    for (JsonVariant v : arr) {
        if (s_student_count >= ROSTER_MAX_STUDENTS) {
            return fail(ROSTER_BAD_STUDENTS, "students.json: more than %d students",
                        ROSTER_MAX_STUDENTS);
        }
        JsonObject st = v.as<JsonObject>();
        if (st.isNull()) {
            return fail(ROSTER_BAD_STUDENTS, "students.json: entry %d is not an object",
                        s_student_count + 1);
        }
        const char* id = st["id"] | "";
        const char* name = st["name"] | "";
        const char* uid = st["rfid_uid"] | "";  // null/absent -> "" (unbound)
        if (id[0] == '\0') {
            return fail(ROSTER_BAD_STUDENTS, "students.json: entry %d has no \"id\"",
                        s_student_count + 1);
        }
        if (name[0] == '\0') {
            return fail(ROSTER_BAD_STUDENTS, "students.json: student %s has no \"name\"", id);
        }
        if (find_student(id) >= 0) {
            return fail(ROSTER_BAD_STUDENTS, "students.json: duplicate id %s", id);
        }

        student_t* dst = &s_students[s_student_count];
        snprintf(dst->id, sizeof(dst->id), "%s", id);
        snprintf(dst->name, sizeof(dst->name), "%s", name);
        snprintf(dst->rfid_uid, sizeof(dst->rfid_uid), "%s", uid);

        // One card belongs to exactly one student; catch copy-paste slips.
        if (dst->rfid_uid[0]) {
            char a[32], b[32];
            uid_normalize(dst->rfid_uid, a, sizeof(a));
            for (int i = 0; i < s_student_count; i++) {
                if (!s_students[i].rfid_uid[0]) continue;
                uid_normalize(s_students[i].rfid_uid, b, sizeof(b));
                if (strcmp(a, b) == 0) {
                    return fail(ROSTER_BAD_STUDENTS,
                                "students.json: %s and %s share RFID uid %s",
                                s_students[i].id, dst->id, dst->rfid_uid);
                }
            }
        }
        s_student_count++;
    }
    return ROSTER_OK;
}

static roster_status_t load_one_class(const char* root, const char* dname) {
    if (s_class_count >= ROSTER_MAX_CLASSES) {
        return fail(ROSTER_BAD_CLASS, "more than %d classes in /classes", ROSTER_MAX_CLASSES);
    }

    char path[128];
    snprintf(path, sizeof(path), "%s%s/%s/class.json", root, CLASSES_DIR, dname);
    if (!SD_MMC.exists(path)) {
        return fail(ROSTER_BAD_CLASS, "classes/%s: class.json is missing", dname);
    }
    File f = SD_MMC.open(path, FILE_READ);
    if (!f) return fail(ROSTER_BAD_CLASS, "classes/%s: cannot open class.json", dname);
    if (f.size() > CLASS_MAX_BYTES) {
        f.close();
        return fail(ROSTER_BAD_CLASS, "classes/%s: class.json too large", dname);
    }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, f);
    f.close();
    if (err) return fail(ROSTER_BAD_CLASS, "classes/%s/class.json: %s", dname, err.c_str());

    class_rec_t* c = &s_classes[s_class_count];
    memset(c, 0, sizeof(*c));

    const char* code = doc["code"] | "";
    const char* name = doc["name"] | "";
    if (code[0] == '\0') {
        return fail(ROSTER_BAD_CLASS, "classes/%s: class.json has no \"code\"", dname);
    }
    if (name[0] == '\0') {
        return fail(ROSTER_BAD_CLASS, "classes/%s: class.json has no \"name\"", dname);
    }
    for (int i = 0; i < s_class_count; i++) {
        if (strcmp(s_classes[i].code, code) == 0) {
            return fail(ROSTER_BAD_CLASS, "classes/%s: duplicate class code %s", dname, code);
        }
    }
    snprintf(c->code, sizeof(c->code), "%s", code);
    snprintf(c->dir, sizeof(c->dir), "%s", dname);
    snprintf(c->name, sizeof(c->name), "%s", name);
    snprintf(c->schedule, sizeof(c->schedule), "%s", (const char*)(doc["schedule"] | ""));
    snprintf(c->teacher_email, sizeof(c->teacher_email), "%s",
             (const char*)(doc["teacher_email"] | ""));
    if (c->teacher_email[0] == '\0') {
        ESP_LOGW(TAG, "classes/%s: no teacher_email, class won't show under any teacher",
                 dname);
    }

    const char* col = doc["color"] | "272766";
    if (col[0] == '#') col++;
    c->color = (uint32_t)strtoul(col, nullptr, 16);

    // Optional per-class photo-capture override: present bool -> 0/1, absent -> -1
    // (inherit the device-wide flag).
    c->capture_photos = doc["capture_photos"].is<bool>() ? (doc["capture_photos"] ? 1 : 0) : -1;
    // Optional timed (double-tap) attendance.
    c->timed_attendance = doc["timed_attendance"] | false;
    c->min_attendance_min = (int16_t)(doc["min_attendance_min"] | 45);

    JsonArray roster = doc["roster"].as<JsonArray>();
    if (roster.isNull()) {
        ESP_LOGW(TAG, "classes/%s: empty roster", dname);
    } else {
        for (JsonVariant v : roster) {
            if (c->roster_count >= ROSTER_MAX_CLASS_STUDENTS) {
                return fail(ROSTER_BAD_CLASS, "classes/%s: more than %d students in roster",
                            dname, ROSTER_MAX_CLASS_STUDENTS);
            }
            // Entries are objects ({"id": ..., "turma": ...}); bare strings too.
            const char* sid = v.is<const char*>() ? v.as<const char*>()
                                                  : (const char*)(v["id"] | "");
            const char* turma = v.is<const char*>() ? "" : (const char*)(v["turma"] | "");
            if (sid == nullptr || sid[0] == '\0') {
                return fail(ROSTER_BAD_CLASS, "classes/%s: roster entry %d has no id", dname,
                            c->roster_count + 1);
            }
            int idx = find_student(sid);
            if (idx < 0) {
                return fail(ROSTER_BAD_CLASS, "classes/%s: unknown student %s in roster",
                            dname, sid);
            }
            for (int i = 0; i < c->roster_count; i++) {
                if (c->roster[i] == idx) {
                    return fail(ROSTER_BAD_CLASS, "classes/%s: student %s listed twice",
                                dname, sid);
                }
            }
            snprintf(c->roster_turma[c->roster_count], sizeof(c->roster_turma[0]), "%s", turma);
            c->roster[c->roster_count++] = (int16_t)idx;
        }
    }

    s_class_count++;
    return ROSTER_OK;
}

static roster_status_t load_classes(const char* root) {
    s_class_count = 0;

    char cdir[48];
    snprintf(cdir, sizeof(cdir), "%s%s", root, CLASSES_DIR);
    File dir = SD_MMC.open(cdir);
    if (!dir || !dir.isDirectory()) {
        // Legitimate first state: a card prepared with students only.
        ESP_LOGW(TAG, "no %s directory, starting with zero classes", cdir);
        if (dir) dir.close();
        return ROSTER_OK;
    }

    File e;
    while ((e = dir.openNextFile())) {
        if (e.isDirectory()) {
            char dname[40];
            snprintf(dname, sizeof(dname), "%s", e.name());
            e.close();
            roster_status_t st = load_one_class(root, dname);
            if (st != ROSTER_OK) {
                dir.close();
                return st;
            }
        } else {
            e.close();
        }
    }
    dir.close();
    return ROSTER_OK;
}

// Loads students + classes from a tree rooted at `root` ("" = the live SD root,
// "/import_staging" = a staged import). Threading the prefix through lets
// roster_validate_tree() reuse the exact loaders against a candidate tree.
static roster_status_t load_all(const char* root) {
    if (!sd_card_mount()) {
        return fail(ROSTER_NO_SD, "No SD card, or card not readable (FAT32 required)");
    }

    roster_status_t st = load_students(root);
    if (st != ROSTER_OK) return st;
    st = load_classes(root);
    if (st != ROSTER_OK) return st;

    int bound = 0;
    for (int i = 0; i < s_student_count; i++) {
        if (s_students[i].rfid_uid[0]) bound++;
    }
    ESP_LOGI(TAG, "loaded %d students (%d with cards), %d classes", s_student_count, bound,
             s_class_count);
    for (int i = 0; i < s_class_count; i++) {
        ESP_LOGI(TAG, "  %s \"%s\": %d students (%s)", s_classes[i].code, s_classes[i].name,
                 s_classes[i].roster_count, s_classes[i].teacher_email);
    }
    s_error[0] = '\0';
    return ROSTER_OK;
}

static roster_status_t locked_load(void) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    roster_status_t st = load_all("");
    xSemaphoreGive(s_lock);
    return st;
}

bool roster_validate_tree(const char* root, char* msg, size_t cap) {
    if (msg && cap) msg[0] = '\0';
    if (!s_lock) {
        if (msg && cap) snprintf(msg, cap, "roster service not started");
        return false;
    }
    // The loaders write the shared arrays, so we borrow them as scratch: load
    // the staged tree, capture the verdict, then reload the live tree to put
    // everything back. The whole dance is under the one lock the enroll writes
    // and the retry task also take, so no other task ever sees the staging data
    // and s_status is left untouched (validation must not change what's
    // published). Live RAM ends matching the live SD files, whatever happens.
    xSemaphoreTake(s_lock, portMAX_DELAY);
    roster_status_t st = load_all(root);
    char captured[128];
    snprintf(captured, sizeof(captured), "%s", s_error);
    load_all("");  // restore live state from the SD root
    xSemaphoreGive(s_lock);

    if (msg && cap) snprintf(msg, cap, "%s", st == ROSTER_OK ? "" : captured);
    return st == ROSTER_OK;
}

static void publish(roster_status_t st) {
    s_status = st;
    app_event_t ev = {};
    ev.type = APP_EVENT_ROSTER_STATE;
    ev.roster.status = (uint8_t)st;
    event_bus_post(&ev);
}

// Retries until the card data is valid, so inserting/fixing the card after
// boot updates the idle screen. Exits once everything loads.
static void roster_task(void*) {
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(RETRY_PERIOD_MS));
        roster_status_t st = locked_load();
        if (st != s_status) publish(st);
        if (st == ROSTER_OK) break;
    }
    vTaskDelete(nullptr);
}

bool roster_service_start(void) {
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return false;

    publish(locked_load());

    if (s_status != ROSTER_OK) {
        xTaskCreate(roster_task, "roster", 6144, nullptr, 2, nullptr);
    }
    return true;
}

void roster_service_reload(void) { publish(locked_load()); }

roster_status_t roster_get_status(void) { return s_status; }

void roster_get_error(char* out, size_t cap) {
    if (!out || !cap) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    snprintf(out, cap, "%s", s_error);
    xSemaphoreGive(s_lock);
}

int roster_class_index(const char* code) {
    if (!code) return -1;
    for (int i = 0; i < s_class_count; i++) {
        if (strcmp(s_classes[i].code, code) == 0) return i;
    }
    return -1;
}

// --- Enrollment writes ------------------------------------------------------

static bool read_json_file(const char* path, JsonDocument& doc) {
    File f = SD_MMC.open(path, FILE_READ);
    if (!f) return false;
    bool ok = !deserializeJson(doc, f);
    f.close();
    return ok;
}

// Writes doc to a temp file then renames over path, so a power cut leaves
// either the old file or the new one — never a half-written file. Serializes
// to a heap buffer first (one write() call), which keeps this portable
// between the device's Print-based File and the native test mock.
static bool write_json_file(const char* path, JsonDocument& doc) {
    size_t need = measureJsonPretty(doc);
    char* buf = (char*)malloc(need + 1);
    if (!buf) return false;
    size_t len = serializeJsonPretty(doc, buf, need + 1);

    char tmp[112];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    File f = SD_MMC.open(tmp, FILE_WRITE, true);
    if (!f) {
        free(buf);
        return false;
    }
    size_t written = f.write((const uint8_t*)buf, len);
    f.close();
    free(buf);
    if (written != len) return false;

    SD_MMC.remove(path);  // FAT rename requires the target to be absent
    return SD_MMC.rename(tmp, path);
}

// Is this normalized UID already assigned to a student other than `except`?
static int uid_owner(const char* norm_uid, int except) {
    char other[32];
    for (int i = 0; i < s_student_count; i++) {
        if (i == except || !s_students[i].rfid_uid[0]) continue;
        uid_normalize(s_students[i].rfid_uid, other, sizeof(other));
        if (strcmp(norm_uid, other) == 0) return i;
    }
    return -1;
}

bool roster_uid_belongs_to_student(const char* uid, char* name_out, size_t cap) {
    if (name_out && cap) name_out[0] = '\0';
    if (!uid || uid[0] == '\0' || roster_get_status() != ROSTER_OK) return false;
    char norm[32];
    uid_normalize(uid, norm, sizeof(norm));
    if (norm[0] == '\0') return false;
    int owner = uid_owner(norm, -1);
    if (owner < 0) return false;
    if (name_out) snprintf(name_out, cap, "%s", s_students[owner].name);
    return true;
}

// Does this normalized UID belong to one of the professors in config.json? A
// student must never be given a card that unlocks the device. Copies the
// professor's name into name_out on a match.
static bool uid_is_professor(const char* norm_uid, char* name_out, size_t cap) {
    device_config_t cfg;
    config_get(&cfg);  // zeroed unless config is loaded
    char other[32];
    for (int i = 0; i < cfg.teacher_count; i++) {
        if (!cfg.teachers[i].rfid_uid[0]) continue;
        uid_normalize(cfg.teachers[i].rfid_uid, other, sizeof(other));
        if (strcmp(norm_uid, other) == 0) {
            if (name_out) snprintf(name_out, cap, "%s", cfg.teachers[i].name);
            return true;
        }
    }
    return false;
}

// Persists rfid_uid onto the student's object in students.json (DOM edit, so
// any fields we don't model are preserved).
static bool persist_student_uid(const char* student_id, const char* uid) {
    JsonDocument doc;
    if (!read_json_file(STUDENTS_PATH, doc)) return false;
    for (JsonObject o : doc["students"].as<JsonArray>()) {
        if (strcmp(o["id"] | "", student_id) == 0) {
            o["rfid_uid"] = uid;
            return write_json_file(STUDENTS_PATH, doc);
        }
    }
    return false;
}

static bool persist_new_student(const char* id, const char* name, const char* uid) {
    JsonDocument doc;
    if (!read_json_file(STUDENTS_PATH, doc)) return false;
    JsonObject o = doc["students"].add<JsonObject>();
    o["id"] = id;
    o["name"] = name;
    o["rfid_uid"] = uid;
    return write_json_file(STUDENTS_PATH, doc);
}

// Appends {"id": student_id, "turma": turma?} to the class roster if absent.
// `turma` may be NULL/empty (no tag written). Returns true if the file is
// consistent afterwards (already-present is success, no write).
static bool persist_enroll(const class_rec_t* cls, const char* student_id, const char* turma) {
    char path[128];
    snprintf(path, sizeof(path), "%s/%s/class.json", CLASSES_DIR, cls->dir);
    JsonDocument doc;
    if (!read_json_file(path, doc)) return false;
    JsonArray roster = doc["roster"].as<JsonArray>();
    if (roster.isNull()) roster = doc["roster"].to<JsonArray>();
    for (JsonVariant v : roster) {
        const char* rid = v.is<const char*>() ? v.as<const char*>() : (const char*)(v["id"] | "");
        if (rid && strcmp(rid, student_id) == 0) return true;  // already enrolled
    }
    JsonObject e = roster.add<JsonObject>();
    e["id"] = student_id;
    if (turma && turma[0]) e["turma"] = turma;
    return write_json_file(path, doc);
}

// Adds student_idx to the in-RAM class roster if not already present, tagging
// the entry with `turma` (may be NULL/empty).
static void ram_enroll(class_rec_t* cls, int student_idx, const char* turma) {
    for (int i = 0; i < cls->roster_count; i++) {
        if (cls->roster[i] == student_idx) return;
    }
    if (cls->roster_count < ROSTER_MAX_CLASS_STUDENTS) {
        int slot = cls->roster_count++;
        cls->roster[slot] = (int16_t)student_idx;
        snprintf(cls->roster_turma[slot], sizeof(cls->roster_turma[0]), "%s", turma ? turma : "");
    }
}

static roster_result_t make_result(bool ok, const char* fmt, ...) {
    roster_result_t r = {ok, ""};
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(r.message, sizeof(r.message), fmt, ap);
    va_end(ap);
    return r;
}

roster_result_t roster_enroll_existing(const char* class_code, int student_idx,
                                       const char* uid) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    roster_result_t r = {false, ""};

    char prof[48];
    if (s_status != ROSTER_OK) {
        r = make_result(false, "Student data not loaded");
    } else if (student_idx < 0 || student_idx >= s_student_count) {
        r = make_result(false, "Unknown student");
    } else {
        char norm[32];
        uid_normalize(uid, norm, sizeof(norm));
        int owner = uid_owner(norm, student_idx);
        int ci = roster_class_index(class_code);
        if (norm[0] == '\0') {
            r = make_result(false, "Empty card ID");
        } else if (owner >= 0) {
            r = make_result(false, "Card already assigned to %s", s_students[owner].name);
        } else if (uid_is_professor(norm, prof, sizeof(prof))) {
            r = make_result(false, "Card belongs to %s", prof);
        } else if (ci < 0) {
            r = make_result(false, "Unknown class");
        } else if (!persist_student_uid(s_students[student_idx].id, uid)) {
            r = make_result(false, "Could not save students.json");
        } else {
            snprintf(s_students[student_idx].rfid_uid,
                     sizeof(s_students[student_idx].rfid_uid), "%s", uid);
            if (!persist_enroll(&s_classes[ci], s_students[student_idx].id, nullptr)) {
                r = make_result(false, "Card saved, but class.json failed");
            } else {
                ram_enroll(&s_classes[ci], student_idx, nullptr);
                r = make_result(true, "Registered %s", s_students[student_idx].name);
            }
        }
    }
    xSemaphoreGive(s_lock);
    return r;
}

roster_result_t roster_enroll_new(const char* class_code, const char* id, const char* name,
                                  const char* uid, const char* turma) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    roster_result_t r = {false, ""};

    char norm[32];
    uid_normalize(uid, norm, sizeof(norm));
    int ci = roster_class_index(class_code);
    int owner = uid_owner(norm, -1);
    char prof[48];

    if (s_status != ROSTER_OK) {
        r = make_result(false, "Student data not loaded");
    } else if (!id || !id[0] || !name || !name[0]) {
        r = make_result(false, "Name and ID are required");
    } else if (turma && strlen(turma) > 15) {  // per CONFIG_IMPORT.md §3.3
        r = make_result(false, "Turma too long (max 15)");
    } else if (find_student(id) >= 0) {
        r = make_result(false, "Student %s already exists", id);
    } else if (norm[0] == '\0') {
        r = make_result(false, "Empty card ID");
    } else if (owner >= 0) {
        r = make_result(false, "Card already assigned to %s", s_students[owner].name);
    } else if (uid_is_professor(norm, prof, sizeof(prof))) {
        r = make_result(false, "Card belongs to %s", prof);
    } else if (ci < 0) {
        r = make_result(false, "Unknown class");
    } else if (s_student_count >= ROSTER_MAX_STUDENTS) {
        r = make_result(false, "Student registry is full");
    } else if (!persist_new_student(id, name, uid)) {
        r = make_result(false, "Could not save students.json");
    } else {
        int idx = s_student_count++;
        snprintf(s_students[idx].id, sizeof(s_students[idx].id), "%s", id);
        snprintf(s_students[idx].name, sizeof(s_students[idx].name), "%s", name);
        snprintf(s_students[idx].rfid_uid, sizeof(s_students[idx].rfid_uid), "%s", uid);
        if (!persist_enroll(&s_classes[ci], id, turma)) {
            r = make_result(false, "Student saved, but class.json failed");
        } else {
            ram_enroll(&s_classes[ci], idx, turma);
            r = make_result(true, "Added %s", name);
        }
    }
    xSemaphoreGive(s_lock);
    return r;
}

bool roster_clear_all_uids(void) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool ok = false;
    if (s_status == ROSTER_OK) {
        JsonDocument doc;
        if (read_json_file(STUDENTS_PATH, doc)) {
            for (JsonObject o : doc["students"].as<JsonArray>()) {
                o["rfid_uid"] = nullptr;  // JSON null = unbound, per the schema
            }
            if (write_json_file(STUDENTS_PATH, doc)) {
                for (int i = 0; i < s_student_count; i++) s_students[i].rfid_uid[0] = '\0';
                ok = true;
                ESP_LOGW(TAG, "DEBUG: cleared all %d student card bindings", s_student_count);
            }
        }
    }
    xSemaphoreGive(s_lock);
    return ok;
}

bool class_capture_enabled(const class_rec_t* cls) {
    if (cls) {
        if (cls->capture_photos == 0) return false;
        if (cls->capture_photos == 1) return true;
    }
    return config_photo_capture_enabled();  // -1 (or NULL class) inherits the device flag
}

roster_result_t roster_class_update_settings(const char* class_code, const char* name,
                                             const char* schedule, uint32_t color, int8_t capture,
                                             bool timed, int min_attendance_min) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    roster_result_t r = {false, ""};

    int ci = roster_class_index(class_code);
    if (s_status != ROSTER_OK) {
        r = make_result(false, "Student data not loaded");
    } else if (ci < 0) {
        r = make_result(false, "Unknown class");
    } else if (!name || !name[0]) {
        r = make_result(false, "Class name is required");
    } else {
        class_rec_t* c = &s_classes[ci];
        char path[128];
        snprintf(path, sizeof(path), "%s/%s/class.json", CLASSES_DIR, c->dir);

        JsonDocument doc;
        if (!read_json_file(path, doc)) {
            r = make_result(false, "Could not read class.json");
        } else {
            char color_hex[8];
            snprintf(color_hex, sizeof(color_hex), "%06X", (unsigned)(color & 0xFFFFFF));
            doc["name"] = name;
            doc["schedule"] = schedule ? schedule : "";
            doc["color"] = color_hex;
            if (capture < 0) {
                doc.remove("capture_photos");  // inherit -> no key
            } else {
                doc["capture_photos"] = (capture != 0);
            }
            if (min_attendance_min < 1) min_attendance_min = 1;
            doc["timed_attendance"] = timed;
            doc["min_attendance_min"] = min_attendance_min;
            if (!write_json_file(path, doc)) {
                r = make_result(false, "Could not save class.json");
            } else {
                snprintf(c->name, sizeof(c->name), "%s", name);
                snprintf(c->schedule, sizeof(c->schedule), "%s", schedule ? schedule : "");
                c->color = color & 0xFFFFFF;
                c->capture_photos = capture;
                c->timed_attendance = timed;
                c->min_attendance_min = (int16_t)min_attendance_min;
                r = make_result(true, "Settings saved");
            }
        }
    }
    xSemaphoreGive(s_lock);
    return r;
}

int roster_student_count(void) { return s_student_count; }
int roster_class_count(void) { return s_class_count; }

const class_rec_t* roster_class_at(int idx) {
    if (s_status != ROSTER_OK || idx < 0 || idx >= s_class_count) return nullptr;
    return &s_classes[idx];
}

const student_t* roster_student_at(int idx) {
    if (s_status != ROSTER_OK || idx < 0 || idx >= s_student_count) return nullptr;
    return &s_students[idx];
}
