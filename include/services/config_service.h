#pragma once

#include <stddef.h>
#include <stdint.h>

#include "app/teacher.h"

// Reads the device's general configuration from /config.json on the root of
// the SD card (FAT32). The file lists the professors authorized to use the
// device, each with an RFID tag and their own password fallback (used when a
// tag isn't available). Every professor's password must be unique so it
// identifies exactly one of them:
//
//   {
//     "teachers": [
//       { "name": "Prof ...", "email": "...",
//         "rfid_uid": "E0:D1:33:5F", "password": "1234" }
//     ]
//   }
//
// The config counts as valid if it has at least one teacher, no two
// professors share a password, and every password is digits-only (so it can
// be typed on the numeric keypads).

constexpr int CONFIG_MAX_TEACHERS = 8;

typedef enum : uint8_t {
    CONFIG_OK = 0,              // SD mounted, config.json parsed and valid
    CONFIG_NO_SD,              // no card, mount failed, or not FAT32
    CONFIG_NO_FILE,            // SD mounted but /config.json is missing
    CONFIG_BAD_JSON,           // config.json unparseable or has no teachers
    CONFIG_DUP_PASSWORD,       // two professors share the same password
    CONFIG_NON_NUMERIC_PASSWORD,  // a password contains non-digit characters
} config_status_t;

typedef struct {
    teacher_t teachers[CONFIG_MAX_TEACHERS];
    int teacher_count;
} device_config_t;

// Attempts a first load synchronously (mount + read + log), then, if that
// didn't succeed, starts a background task that keeps retrying so inserting a
// card later updates the UI. Publishes APP_EVENT_CONFIG_STATE on every state
// change. Call once in setup(), after event_bus_init() and before ui_init()
// so the idle screen's first frame reflects the real state.
bool config_service_start(void);

// Re-reads /config.json and republishes the status. For the importer to call
// after applying a new config. LVGL/import thread.
void config_service_reload(void);

// Current status. Safe to read from any task.
config_status_t config_get_status(void);

// Human-readable reason for the current failure (e.g. "Prof A and Prof B
// share the same password"). Empty string when status is CONFIG_OK.
// Thread-safe.
void config_get_error(char* out, size_t cap);

// True if at least one professor has a password set. When false, password
// login is impossible and the idle screen hides the "Enter password" button.
// Thread-safe.
bool config_has_any_password(void);

// Copies the loaded config into out. Meaningful only when the status is
// CONFIG_OK; otherwise out is zeroed. Thread-safe.
void config_get(device_config_t* out);

// Single-teacher lookups. They iterate the loaded config under the lock and
// copy only the matched teacher into `out` (may be NULL), so callers don't put
// a whole device_config_t (~1.5 kB) on their stack. All return false unless the
// config status is CONFIG_OK. Thread-safe.
//
// - by_uid: matches the configured rfid_uid, ignoring case and separators.
// - by_password: matches the exact (unique) password; empty never matches.
bool config_find_teacher_by_uid(const char* uid_hex, teacher_t* out);
bool config_find_teacher_by_password(const char* password, teacher_t* out);

// True if the teacher identified by `email` (or `rfid_uid` when email is empty)
// has a non-empty password. Thread-safe.
bool config_teacher_has_password(const char* email, const char* rfid_uid);

// Outcome of a config write, with a human-readable reason on failure.
typedef struct {
    bool ok;
    char message[96];
} config_result_t;

// Writes a fresh /config.json holding a single professor — the on-device
// bootstrap for a card that has no configuration at all. Without it a device
// handed over with a blank card cannot be set up on its own: unlocking needs a
// professor, and the WiFi file manager that would receive a config.tar sits
// behind the unlock gate.
//
// Refuses unless the status is exactly CONFIG_NO_FILE, so it can never
// overwrite a valid config nor paper over a broken one — a malformed
// config.json must be repaired, not silently replaced. The new professor gets
// no email and no card: an empty email sees every class
// (roster_class_matches_teacher), which is what a setup account needs, and the
// real professors arrive with the config.tar that overwrites this file.
//
// Returns {ok,message} — on failure nothing is written. Call on the UI/LVGL
// thread (does SD I/O).
config_result_t config_create_first_teacher(const char* name, const char* password);

// Sets (or adds, if absent) the password for the professor identified by
// `email` (falls back to `rfid_uid` when email is empty), rewriting
// /config.json atomically and reloading. The new password must be non-empty,
// digits-only, and not already used by another professor. Returns {ok,message}
// — on failure nothing is written. Call on the UI/LVGL thread (does SD I/O).
config_result_t config_set_password(const char* email, const char* rfid_uid,
                                    const char* new_password);

// Sets (or replaces) the RFID card UID of the professor identified by `email`
// (falls back to `rfid_uid` when email is empty), rewriting /config.json
// atomically and reloading. The new UID must be non-empty and, once
// canonicalized (see uid_normalize), must not already be bound to another
// professor. Returns {ok,message} — on failure nothing is written. Call on the
// UI/LVGL thread (does SD I/O).
config_result_t config_set_rfid(const char* email, const char* rfid_uid, const char* new_uid);

// Validates a staged config tree rooted at `root` (e.g. "/import_staging") with
// the exact live rules, WITHOUT mutating shared state — used by the config
// importer to check a tar before applying it. Reads "<root>/config.json".
// Returns true when valid; on failure fills `msg` with the reason (empty on
// success). Writes no shared state; call on the import (LVGL) thread (SD I/O).
bool config_validate_tree(const char* root, char* msg, size_t cap);
