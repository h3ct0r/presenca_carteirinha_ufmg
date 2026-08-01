// config_service + auth against the in-memory SD card: teacher list parsing,
// per-professor passwords (must be unique), status codes, UID/password lookup.
//
// Note on ordering: sd_card_mount() latches "mounted" for the whole binary,
// so the mount-failure scenario MUST run first. The auth lookup tests rely on
// the config loaded by test_valid_config_parses_teachers just before them.

#include <stdio.h>
#include <string.h>
#include <unity.h>

#include "app/auth.h"
#include "app/event_bus.h"
#include "mock_freertos.h"
#include "mock_sd.h"
#include "services/config_service.h"
#include "services/roster_service.h"

void setUp(void) {
    app_event_t ev;
    while (event_bus_poll(&ev)) {
    }
}
void tearDown(void) {}

static const char* VALID_CONFIG =
    "{\n"
    "  \"teachers\": [\n"
    "    { \"name\": \"Prof Hector Azpurua\", \"email\": \"hector@dcc.ufmg.br\",\n"
    "      \"rfid_uid\": \"E0:D1:33:5F\", \"password\": \"1234\" },\n"
    "    { \"name\": \"Prof Two\", \"email\": \"two@dcc.ufmg.br\",\n"
    "      \"rfid_uid\": \"E0:D1:33:6F\", \"password\": \"56789\" }\n"
    "  ]\n"
    "}\n";

// Last CONFIG_STATE event on the bus, or -1.
static int last_published_config_status(void) {
    int status = -1;
    app_event_t ev;
    while (event_bus_poll(&ev)) {
        if (ev.type == APP_EVENT_CONFIG_STATE) status = ev.config.status;
    }
    return status;
}

static void expect_error_contains(const char* needle) {
    char msg[128];
    config_get_error(msg, sizeof(msg));
    if (strstr(msg, needle) == nullptr) {
        char fail[256];
        snprintf(fail, sizeof(fail), "error \"%s\" does not contain \"%s\"", msg, needle);
        TEST_FAIL_MESSAGE(fail);
    }
}

static void test_no_sd_card(void) {
    mocksd_reset();
    mocksd_set_begin_result(false);
    config_service_start();
    TEST_ASSERT_EQUAL(CONFIG_NO_SD, config_get_status());
    TEST_ASSERT_EQUAL_INT(CONFIG_NO_SD, last_published_config_status());
    // Locked: nothing to authorize against.
    TEST_ASSERT_FALSE(auth_lookup_uid("E0:D1:33:5F", nullptr));
    TEST_ASSERT_FALSE(auth_lookup_password("1234", nullptr));
}

static void test_missing_config_file(void) {
    mocksd_reset();  // begin ok, empty card
    config_service_start();
    TEST_ASSERT_EQUAL(CONFIG_NO_FILE, config_get_status());
}

static void test_unparseable_json(void) {
    mocksd_reset();
    mocksd_add_file("/config.json", "{ not json");
    config_service_start();
    TEST_ASSERT_EQUAL(CONFIG_BAD_JSON, config_get_status());
}

static void test_no_teachers_is_invalid(void) {
    mocksd_reset();
    mocksd_add_file("/config.json", "{ \"teachers\": [] }");
    config_service_start();
    TEST_ASSERT_EQUAL(CONFIG_BAD_JSON, config_get_status());
    expect_error_contains("no teachers");
}

static void test_duplicate_password_rejected(void) {
    mocksd_reset();
    mocksd_add_file("/config.json",
                    "{ \"teachers\": [\n"
                    "  { \"name\": \"Prof A\", \"rfid_uid\": \"AA:00\", \"password\": \"1111\" },\n"
                    "  { \"name\": \"Prof B\", \"rfid_uid\": \"BB:00\", \"password\": \"1111\" }\n"
                    "] }");
    config_service_start();
    TEST_ASSERT_EQUAL(CONFIG_DUP_PASSWORD, config_get_status());
    TEST_ASSERT_EQUAL_INT(CONFIG_DUP_PASSWORD, last_published_config_status());
    // The message must name both conflicting professors so the fix is obvious.
    expect_error_contains("Prof A");
    expect_error_contains("Prof B");
}

static void test_non_numeric_password_rejected(void) {
    mocksd_reset();
    mocksd_add_file("/config.json",
                    "{ \"teachers\": [\n"
                    "  { \"name\": \"Prof A\", \"rfid_uid\": \"AA:00\", \"password\": \"12ab\" }\n"
                    "] }");
    config_service_start();
    TEST_ASSERT_EQUAL(CONFIG_NON_NUMERIC_PASSWORD, config_get_status());
    TEST_ASSERT_EQUAL_INT(CONFIG_NON_NUMERIC_PASSWORD, last_published_config_status());
    // The message must name the offending professor so the fix is obvious.
    expect_error_contains("Prof A");
}

static void test_empty_passwords_do_not_collide(void) {
    // RFID-only professors (no password) are fine, even several of them.
    mocksd_reset();
    mocksd_add_file("/config.json",
                    "{ \"teachers\": [\n"
                    "  { \"name\": \"Prof A\", \"rfid_uid\": \"AA:00\" },\n"
                    "  { \"name\": \"Prof B\", \"rfid_uid\": \"BB:00\" }\n"
                    "] }");
    config_service_start();
    TEST_ASSERT_EQUAL(CONFIG_OK, config_get_status());
    TEST_ASSERT_FALSE(auth_lookup_password("", nullptr));
    // No passwords anywhere -> idle screen hides the password button.
    TEST_ASSERT_FALSE(config_has_any_password());
}

static void test_valid_config_parses_teachers(void) {
    mocksd_reset();
    mocksd_add_file("/config.json", VALID_CONFIG);
    config_service_start();
    TEST_ASSERT_EQUAL(CONFIG_OK, config_get_status());
    TEST_ASSERT_EQUAL_INT(CONFIG_OK, last_published_config_status());

    // Error string cleared on success.
    char err[128];
    config_get_error(err, sizeof(err));
    TEST_ASSERT_EQUAL_STRING("", err);

    device_config_t cfg;
    config_get(&cfg);
    TEST_ASSERT_EQUAL_INT(2, cfg.teacher_count);
    TEST_ASSERT_EQUAL_STRING("Prof Hector Azpurua", cfg.teachers[0].name);
    TEST_ASSERT_EQUAL_STRING("two@dcc.ufmg.br", cfg.teachers[1].email);
    TEST_ASSERT_EQUAL_STRING("1234", cfg.teachers[0].password);
    TEST_ASSERT_EQUAL_STRING("56789", cfg.teachers[1].password);
    TEST_ASSERT_TRUE(config_has_any_password());
}

static void test_uid_lookup_identifies_teacher(void) {
    teacher_t who = {};
    TEST_ASSERT_TRUE(auth_lookup_uid("E0:D1:33:5F", &who));
    TEST_ASSERT_EQUAL_STRING("Prof Hector Azpurua", who.name);

    // Formatting must not matter.
    TEST_ASSERT_TRUE(auth_lookup_uid("e0d1335f", &who));
    TEST_ASSERT_TRUE(auth_lookup_uid("e0-d1-33-5f", &who));

    TEST_ASSERT_TRUE(auth_lookup_uid("E0:D1:33:6F", &who));
    TEST_ASSERT_EQUAL_STRING("Prof Two", who.name);

    TEST_ASSERT_FALSE(auth_lookup_uid("DE:AD:BE:EF", &who));
    TEST_ASSERT_FALSE(auth_lookup_uid("", &who));
}

static void test_password_lookup_identifies_teacher(void) {
    teacher_t who = {};
    TEST_ASSERT_TRUE(auth_lookup_password("1234", &who));
    TEST_ASSERT_EQUAL_STRING("Prof Hector Azpurua", who.name);

    TEST_ASSERT_TRUE(auth_lookup_password("56789", &who));
    TEST_ASSERT_EQUAL_STRING("Prof Two", who.name);

    TEST_ASSERT_FALSE(auth_lookup_password("wrong", &who));
    TEST_ASSERT_FALSE(auth_lookup_password("", &who));
}

static void test_teacher_has_password(void) {
    // Uses the valid config loaded above; both teachers have a password.
    TEST_ASSERT_TRUE(config_teacher_has_password("hector@dcc.ufmg.br", ""));
    TEST_ASSERT_TRUE(config_teacher_has_password("", "E0:D1:33:5F"));  // rfid_uid fallback
    TEST_ASSERT_FALSE(config_teacher_has_password("nobody@x.edu", ""));
    // by_uid copies out only the matched teacher.
    teacher_t who = {};
    TEST_ASSERT_TRUE(config_find_teacher_by_uid("e0d1335f", &who));
    TEST_ASSERT_EQUAL_STRING("Prof Hector Azpurua", who.name);
}

static void test_teacher_list_is_capped(void) {
    char json[2048];
    int n = snprintf(json, sizeof(json), "{ \"teachers\": [");
    for (int i = 0; i < CONFIG_MAX_TEACHERS + 2; i++) {
        n += snprintf(json + n, sizeof(json) - n,
                      "%s{ \"name\": \"T%d\", \"rfid_uid\": \"00:00:00:%02X\" }",
                      i ? "," : "", i, i);
    }
    snprintf(json + n, sizeof(json) - n, "] }");

    mocksd_reset();
    mocksd_add_file("/config.json", json);
    config_service_start();
    TEST_ASSERT_EQUAL(CONFIG_OK, config_get_status());
    device_config_t cfg;
    config_get(&cfg);
    TEST_ASSERT_EQUAL_INT(CONFIG_MAX_TEACHERS, cfg.teacher_count);
}

// --- config_set_password (Admin panel password editing) ---------------------
// These run last and chain: the first loads a config and adds a password; the
// rest act on the config left loaded by the one before.

static void test_set_password_adds_when_absent(void) {
    mocksd_reset();
    mocksd_add_file(
        "/config.json",
        // Prof B keeps a 4-digit password on purpose: it is shorter than
        // CONFIG_MIN_PASSWORD_DIGITS and must still authenticate, because the
        // floor applies to what the device writes, not to what it reads.
        "{ \"teachers\": [\n"
        "  { \"name\": \"Prof A\", \"email\": \"a@x\", \"rfid_uid\": \"AA:00\" },\n"
        "  { \"name\": \"Prof B\", \"email\": \"b@x\", \"rfid_uid\": \"BB:00\", \"password\": \"1234\" },\n"
        "  { \"name\": \"Prof C\", \"email\": \"c@x\", \"rfid_uid\": \"CC:00\", \"password\": \"123456\" }\n"
        "] }");
    config_service_start();
    TEST_ASSERT_EQUAL(CONFIG_OK, config_get_status());
    TEST_ASSERT_FALSE(auth_lookup_password("567890", nullptr));

    config_result_t r = config_set_password("a@x", "AA:00", "567890");
    TEST_ASSERT_TRUE(r.ok);
    // Reloaded and persisted: the new password now identifies Prof A.
    teacher_t who = {};
    TEST_ASSERT_TRUE(auth_lookup_password("567890", &who));
    TEST_ASSERT_EQUAL_STRING("Prof A", who.name);
}

static void test_set_password_rejects_duplicate(void) {
    // Prof C already uses 123456; Prof A must not be allowed to take it.
    config_result_t r = config_set_password("a@x", "AA:00", "123456");
    TEST_ASSERT_FALSE(r.ok);
    TEST_ASSERT_NOT_NULL(strstr(r.message, "another professor"));
    TEST_ASSERT_TRUE(auth_lookup_password("567890", nullptr));  // A's password unchanged
}

static void test_set_password_rejects_non_numeric(void) {
    config_result_t r = config_set_password("a@x", "AA:00", "12ab");
    TEST_ASSERT_FALSE(r.ok);
}

// The length floor applies to what the device WRITES.
static void test_set_password_rejects_too_short(void) {
    config_result_t r = config_set_password("a@x", "AA:00", "1234");
    TEST_ASSERT_FALSE(r.ok);
    TEST_ASSERT_NOT_NULL(strstr(r.message, "at least"));
    TEST_ASSERT_TRUE(auth_lookup_password("567890", nullptr));  // unchanged
}

// ...but NOT to what it reads. Prof B's 4-digit password was authored into
// config.json and must keep working: raising the floor must never lock a
// professor out of a device that was already set up.
static void test_short_password_already_on_the_card_still_works(void) {
    teacher_t who = {};
    TEST_ASSERT_TRUE(auth_lookup_password("1234", &who));
    TEST_ASSERT_EQUAL_STRING("Prof B", who.name);
}

static void test_change_password_replaces_old(void) {
    config_result_t r = config_set_password("a@x", "AA:00", "999900");
    TEST_ASSERT_TRUE(r.ok);
    TEST_ASSERT_FALSE(auth_lookup_password("567890", nullptr));  // old one gone
    teacher_t who = {};
    TEST_ASSERT_TRUE(auth_lookup_password("999900", &who));
    TEST_ASSERT_EQUAL_STRING("Prof A", who.name);
}

// --- config_set_rfid (Admin panel card editing) -----------------------------
// Continues the chain: config currently has Prof A (a@x, AA:00) and
// Prof B (b@x, BB:00).

static void test_set_rfid_changes_card(void) {
    config_result_t r = config_set_rfid("a@x", "AA:00", "CC:11:22:33");
    TEST_ASSERT_TRUE(r.ok);
    // The new card now identifies Prof A; the old one no longer matches anyone.
    teacher_t who = {};
    TEST_ASSERT_TRUE(config_find_teacher_by_uid("CC:11:22:33", &who));
    TEST_ASSERT_EQUAL_STRING("Prof A", who.name);
    TEST_ASSERT_FALSE(config_find_teacher_by_uid("AA:00", nullptr));
}

static void test_set_rfid_rejects_duplicate(void) {
    // Prof B carries BB:00; Prof A must not be allowed to take it, even when it
    // is written in a different but equivalent format.
    config_result_t r = config_set_rfid("a@x", "CC:11:22:33", "bb-00");
    TEST_ASSERT_FALSE(r.ok);
    TEST_ASSERT_NOT_NULL(strstr(r.message, "another professor"));
    // Prof A's card is unchanged.
    TEST_ASSERT_TRUE(config_find_teacher_by_uid("CC:11:22:33", nullptr));
}

static void test_set_rfid_rejects_empty(void) {
    config_result_t r = config_set_rfid("a@x", "CC:11:22:33", "");
    TEST_ASSERT_FALSE(r.ok);
    TEST_ASSERT_TRUE(config_find_teacher_by_uid("CC:11:22:33", nullptr));
}

// Self-contained: seeds both a config and a student roster so the cross-service
// "is this card a student's?" guard can be exercised.
static void test_set_rfid_rejects_student_card(void) {
    mocksd_reset();
    mocksd_add_file("/config.json",
                    "{ \"teachers\": [\n"
                    "  { \"name\": \"Prof A\", \"email\": \"a@x\", \"rfid_uid\": \"AA:00\" }\n"
                    "] }");
    mocksd_add_file("/students/students.json",
                    "{ \"version\": 1, \"students\": [\n"
                    "  { \"id\": \"2023-0142\", \"name\": \"Maria Santos\", "
                    "\"rfid_uid\": \"04:A3:1B:2C\" }\n"
                    "] }");
    config_service_start();
    roster_service_start();
    TEST_ASSERT_EQUAL(CONFIG_OK, config_get_status());
    TEST_ASSERT_EQUAL(ROSTER_OK, roster_get_status());

    // Maria's card (in a different format) is refused, naming her.
    config_result_t r = config_set_rfid("a@x", "AA:00", "04-a3-1b-2c");
    TEST_ASSERT_FALSE(r.ok);
    TEST_ASSERT_NOT_NULL(strstr(r.message, "Maria Santos"));
    TEST_ASSERT_FALSE(config_find_teacher_by_uid("04:A3:1B:2C", nullptr));

    // A card that belongs to nobody is still accepted.
    config_result_t ok = config_set_rfid("a@x", "AA:00", "DE:AD:BE:EF");
    TEST_ASSERT_TRUE(ok.ok);
}

// --- config_validate_tree (import: validate a staged tree, no live mutation) -

static void test_validate_tree_accepts_good_staging(void) {
    mocksd_reset();
    mocksd_add_file("/config.json", VALID_CONFIG);  // live config (2 teachers)
    mocksd_add_file("/import_staging/config.json",
                    "{ \"teachers\": [ { \"name\": \"Prof New\", \"email\": \"new@x\", "
                    "\"rfid_uid\": \"AB:CD\", \"password\": \"321\" } ] }");
    config_service_start();
    TEST_ASSERT_EQUAL(CONFIG_OK, config_get_status());

    char msg[128] = "dirty";
    TEST_ASSERT_TRUE(config_validate_tree("/import_staging", msg, sizeof(msg)));
    TEST_ASSERT_EQUAL_STRING("", msg);  // cleared on success
}

static void test_validate_tree_rejects_bad_staging_leaving_live_intact(void) {
    // Live is still VALID_CONFIG from the test above (2 teachers, OK).
    mocksd_add_file("/import_staging/config.json",
                    "{ \"teachers\": [ { \"name\": \"A\", \"email\": \"a\", \"password\": \"9\" },"
                    " { \"name\": \"B\", \"email\": \"b\", \"password\": \"9\" } ] }");
    char msg[128];
    TEST_ASSERT_FALSE(config_validate_tree("/import_staging", msg, sizeof(msg)));
    TEST_ASSERT_NOT_NULL(strstr(msg, "share the same password"));

    // The live config is untouched: still OK, still the two original teachers.
    TEST_ASSERT_EQUAL(CONFIG_OK, config_get_status());
    device_config_t cfg;
    config_get(&cfg);
    TEST_ASSERT_EQUAL(2, cfg.teacher_count);
    TEST_ASSERT_TRUE(config_find_teacher_by_password("1234", nullptr));  // original still valid
}

static void test_validate_tree_reports_missing_config(void) {
    char msg[128];
    TEST_ASSERT_FALSE(config_validate_tree("/no_such_root", msg, sizeof(msg)));
    TEST_ASSERT_NOT_NULL(strstr(msg, "not found"));
}

// --- config_create_first_teacher (blank-card bootstrap) ---------------------
// The idle gate offers this only when config.json is absent; the guard inside
// the service is what actually enforces it. These run self-contained (own
// mocksd_reset) and chain in pairs.

static void test_first_teacher_bootstraps_blank_card(void) {
    mocksd_reset();  // empty card: mounts, but no config.json
    config_service_start();
    TEST_ASSERT_EQUAL(CONFIG_NO_FILE, config_get_status());

    // Too short is refused before anything is written — the bootstrap runs the
    // same check_password() the Admin editor does.
    TEST_ASSERT_FALSE(config_create_first_teacher("Prof Setup", "4321").ok);
    TEST_ASSERT_FALSE(mocksd_exists("/config.json"));

    config_result_t r = config_create_first_teacher("Prof Setup", "432100");
    TEST_ASSERT_TRUE(r.ok);

    // Written, reloaded, and immediately usable to unlock the device.
    TEST_ASSERT_TRUE(mocksd_exists("/config.json"));
    TEST_ASSERT_EQUAL(CONFIG_OK, config_get_status());
    teacher_t who = {};
    TEST_ASSERT_TRUE(auth_lookup_password("432100", &who));
    TEST_ASSERT_EQUAL_STRING("Prof Setup", who.name);
    // No email: a setup account sees every class (roster_class_matches_teacher).
    TEST_ASSERT_EQUAL_STRING("", who.email);
}

static void test_first_teacher_refused_when_config_exists(void) {
    // Chains from the test above: the card now holds a valid config.
    TEST_ASSERT_EQUAL(CONFIG_OK, config_get_status());

    config_result_t r = config_create_first_teacher("Intruder", "111100");
    TEST_ASSERT_FALSE(r.ok);
    TEST_ASSERT_NOT_NULL(strstr(r.message, "already has a configuration"));

    // Nothing overwritten: the bootstrap professor is still the only one.
    TEST_ASSERT_FALSE(auth_lookup_password("111100", nullptr));
    TEST_ASSERT_TRUE(auth_lookup_password("432100", nullptr));
}

static void test_first_teacher_rejects_bad_input(void) {
    mocksd_reset();
    config_service_start();
    TEST_ASSERT_EQUAL(CONFIG_NO_FILE, config_get_status());

    TEST_ASSERT_FALSE(config_create_first_teacher("", "1234").ok);          // no name
    TEST_ASSERT_FALSE(config_create_first_teacher("Prof A", "").ok);        // no password
    TEST_ASSERT_FALSE(config_create_first_teacher("Prof A", "12ab").ok);    // not digits
    TEST_ASSERT_FALSE(config_create_first_teacher(nullptr, nullptr).ok);    // null-safe

    // Every rejection is total: nothing written, the card still needs setup.
    TEST_ASSERT_FALSE(mocksd_exists("/config.json"));
    TEST_ASSERT_EQUAL(CONFIG_NO_FILE, config_get_status());
}

// A config.json that exists but does not parse must be repaired, never silently
// replaced — otherwise the bootstrap becomes a way to discard a real config.
static void test_first_teacher_refused_on_broken_config(void) {
    mocksd_reset();
    mocksd_add_file("/config.json", "{ not json");
    config_service_start();
    TEST_ASSERT_EQUAL(CONFIG_BAD_JSON, config_get_status());

    config_result_t r = config_create_first_teacher("Prof A", "1234");
    TEST_ASSERT_FALSE(r.ok);
    TEST_ASSERT_EQUAL(CONFIG_BAD_JSON, config_get_status());
}

int main(int, char**) {
    // Park the services' background retry loops far beyond the test run.
    mock_freertos_set_delay_scale(1000);
    event_bus_init();
    UNITY_BEGIN();
    RUN_TEST(test_no_sd_card);  // must stay first (mount latches)
    RUN_TEST(test_missing_config_file);
    RUN_TEST(test_unparseable_json);
    RUN_TEST(test_no_teachers_is_invalid);
    RUN_TEST(test_duplicate_password_rejected);
    RUN_TEST(test_non_numeric_password_rejected);
    RUN_TEST(test_empty_passwords_do_not_collide);
    RUN_TEST(test_valid_config_parses_teachers);
    RUN_TEST(test_uid_lookup_identifies_teacher);       // uses the valid config above
    RUN_TEST(test_password_lookup_identifies_teacher);  // uses the valid config above
    RUN_TEST(test_teacher_has_password);                // uses the valid config above
    RUN_TEST(test_teacher_list_is_capped);
    RUN_TEST(test_set_password_adds_when_absent);  // chain: loads its own config
    RUN_TEST(test_set_password_rejects_duplicate);
    RUN_TEST(test_set_password_rejects_non_numeric);
    RUN_TEST(test_set_password_rejects_too_short);
    RUN_TEST(test_short_password_already_on_the_card_still_works);
    RUN_TEST(test_change_password_replaces_old);
    RUN_TEST(test_set_rfid_changes_card);      // chain continues from the password tests
    RUN_TEST(test_set_rfid_rejects_duplicate);
    RUN_TEST(test_set_rfid_rejects_empty);
    RUN_TEST(test_set_rfid_rejects_student_card);    // self-contained (own config + roster)
    RUN_TEST(test_validate_tree_accepts_good_staging);          // loads its own live + staging
    RUN_TEST(test_validate_tree_rejects_bad_staging_leaving_live_intact);  // chains from above
    RUN_TEST(test_validate_tree_reports_missing_config);
    RUN_TEST(test_first_teacher_bootstraps_blank_card);   // self-contained (blank card)
    RUN_TEST(test_first_teacher_refused_when_config_exists);  // chains from above
    RUN_TEST(test_first_teacher_rejects_bad_input);
    RUN_TEST(test_first_teacher_refused_on_broken_config);        // chains from above
    return UNITY_END();
}
