// roster_service against the in-memory SD card: layout validation of
// /students/students.json and /classes/<code>/class.json, with the exact
// error messages the idle screen displays.
//
// Ordering: the mount-failure scenario must run first (mount latches for the
// whole binary, see storage/sd_card.cpp).

#include <stdio.h>
#include <string.h>
#include <unity.h>

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

static const char* STUDENTS_JSON =
    "{ \"version\": 1, \"students\": [\n"
    "  { \"id\": \"2023-0142\", \"name\": \"Maria Santos\", \"rfid_uid\": \"04:A3:1B:2C\" },\n"
    "  { \"id\": \"2023-0187\", \"name\": \"John Miller\", \"rfid_uid\": null },\n"
    "  { \"id\": \"2024-0021\", \"name\": \"Lucas Ferreira\" }\n"
    "] }";

// Roster entries carry an optional per-student "turma" tag; the loader reads
// only "id" and ignores turma (it's preserved on rewrite via a DOM edit).
static const char* CLASS_CS101 =
    "{ \"version\": 1, \"code\": \"CS101-M1\", \"name\": \"Data Structures\",\n"
    "  \"schedule\": \"Tue/Thu 10:00-12:00\", \"teacher_email\": \"h@x.edu\",\n"
    "  \"color\": \"272766\",\n"
    "  \"roster\": [ { \"id\": \"2023-0142\", \"turma\": \"TE1\" }, { \"id\": \"2023-0187\", \"turma\": \"TE2\" } ] }";

static void expect_error_contains(const char* needle) {
    char msg[160];
    roster_get_error(msg, sizeof(msg));
    if (strstr(msg, needle) == nullptr) {
        char fail[256];
        snprintf(fail, sizeof(fail), "error \"%s\" does not contain \"%s\"", msg, needle);
        TEST_FAIL_MESSAGE(fail);
    }
}

static void test_no_sd_card(void) {
    mocksd_reset();
    mocksd_set_begin_result(false);
    roster_service_start();
    TEST_ASSERT_EQUAL(ROSTER_NO_SD, roster_get_status());
}

static void test_missing_students_file(void) {
    mocksd_reset();
    roster_service_start();
    TEST_ASSERT_EQUAL(ROSTER_NO_STUDENTS_FILE, roster_get_status());
    expect_error_contains("students.json");
}

static void test_bad_students_json_syntax(void) {
    mocksd_reset();
    mocksd_add_file("/students/students.json", "{ oops");
    roster_service_start();
    TEST_ASSERT_EQUAL(ROSTER_BAD_STUDENTS, roster_get_status());
    expect_error_contains("students.json");
}

static void test_student_missing_name(void) {
    mocksd_reset();
    mocksd_add_file("/students/students.json",
                    "{ \"students\": [ { \"id\": \"2023-0001\" } ] }");
    roster_service_start();
    TEST_ASSERT_EQUAL(ROSTER_BAD_STUDENTS, roster_get_status());
    expect_error_contains("2023-0001");
    expect_error_contains("name");
}

static void test_duplicate_student_id(void) {
    mocksd_reset();
    mocksd_add_file("/students/students.json",
                    "{ \"students\": [\n"
                    "  { \"id\": \"2023-0001\", \"name\": \"A\" },\n"
                    "  { \"id\": \"2023-0001\", \"name\": \"B\" } ] }");
    roster_service_start();
    TEST_ASSERT_EQUAL(ROSTER_BAD_STUDENTS, roster_get_status());
    expect_error_contains("duplicate id");
}

static void test_duplicate_uid_across_formats(void) {
    // Same card written in two formats must still be caught.
    mocksd_reset();
    mocksd_add_file("/students/students.json",
                    "{ \"students\": [\n"
                    "  { \"id\": \"2023-0001\", \"name\": \"A\", \"rfid_uid\": \"e0-d1-33-5f\" },\n"
                    "  { \"id\": \"2023-0002\", \"name\": \"B\", \"rfid_uid\": \"E0:D1:33:5F\" }\n"
                    "] }");
    roster_service_start();
    TEST_ASSERT_EQUAL(ROSTER_BAD_STUDENTS, roster_get_status());
    expect_error_contains("share RFID uid");
}

static void test_class_with_unknown_student(void) {
    mocksd_reset();
    mocksd_add_file("/students/students.json", STUDENTS_JSON);
    mocksd_add_file("/classes/CS101-M1/class.json",
                    "{ \"code\": \"CS101-M1\", \"name\": \"DS\",\n"
                    "  \"roster\": [ { \"id\": \"9999-9999\" } ] }");
    roster_service_start();
    TEST_ASSERT_EQUAL(ROSTER_BAD_CLASS, roster_get_status());
    expect_error_contains("CS101-M1");
    expect_error_contains("unknown student 9999-9999");
}

// A student may hold only one turma per class, so the loader must reject a
// roster that lists the same id twice even when the turma tags differ.
static void test_class_student_twice_with_different_turmas(void) {
    mocksd_reset();
    mocksd_add_file("/students/students.json", STUDENTS_JSON);
    mocksd_add_file("/classes/CS101-M1/class.json",
                    "{ \"code\": \"CS101-M1\", \"name\": \"DS\",\n"
                    "  \"roster\": [ { \"id\": \"2023-0142\", \"turma\": \"TE1\" },\n"
                    "                { \"id\": \"2023-0142\", \"turma\": \"TE2\" } ] }");
    roster_service_start();
    TEST_ASSERT_EQUAL(ROSTER_BAD_CLASS, roster_get_status());
    expect_error_contains("CS101-M1");
    expect_error_contains("listed twice");
}

static void test_class_dir_without_class_json(void) {
    mocksd_reset();
    mocksd_add_file("/students/students.json", STUDENTS_JSON);
    mocksd_add_dir("/classes/CS205-T2");
    roster_service_start();
    TEST_ASSERT_EQUAL(ROSTER_BAD_CLASS, roster_get_status());
    expect_error_contains("CS205-T2");
    expect_error_contains("class.json is missing");
}

static void test_no_classes_dir_is_ok(void) {
    mocksd_reset();
    mocksd_add_file("/students/students.json", STUDENTS_JSON);
    roster_service_start();
    TEST_ASSERT_EQUAL(ROSTER_OK, roster_get_status());
    TEST_ASSERT_EQUAL_INT(3, roster_student_count());
    TEST_ASSERT_EQUAL_INT(0, roster_class_count());
}

static void test_valid_full_layout(void) {
    mocksd_reset();
    mocksd_add_file("/students/students.json", STUDENTS_JSON);
    mocksd_add_file("/classes/CS101-M1/class.json", CLASS_CS101);
    // Bare-string roster entries are accepted too.
    mocksd_add_file("/classes/MA110-F1/class.json",
                    "{ \"code\": \"MA110-F1\", \"name\": \"Linear Algebra\",\n"
                    "  \"teacher_email\": \"other@x.edu\",\n"
                    "  \"roster\": [ \"2024-0021\", \"2023-0142\" ] }");
    roster_service_start();
    TEST_ASSERT_EQUAL(ROSTER_OK, roster_get_status());
    TEST_ASSERT_EQUAL_INT(3, roster_student_count());
    TEST_ASSERT_EQUAL_INT(2, roster_class_count());

    char msg[64];
    roster_get_error(msg, sizeof(msg));
    TEST_ASSERT_EQUAL_STRING("", msg);
}

// Reads back what the screens consume. Relies on the previous test's data
// (classes enumerate in directory order: CS101-M1 then MA110-F1).
static void test_accessors_expose_loaded_data(void) {
    const class_rec_t* cs101 = roster_class_at(0);
    TEST_ASSERT_NOT_NULL(cs101);
    TEST_ASSERT_EQUAL_STRING("CS101-M1", cs101->code);
    TEST_ASSERT_EQUAL_STRING("Data Structures", cs101->name);
    TEST_ASSERT_EQUAL_STRING("h@x.edu", cs101->teacher_email);
    TEST_ASSERT_EQUAL_HEX32(0x272766, cs101->color);
    // A roster of {"id","turma"} objects loads fine, turma tags into RAM.
    TEST_ASSERT_EQUAL_INT(2, cs101->roster_count);
    TEST_ASSERT_EQUAL_STRING("TE1", cs101->roster_turma[0]);
    TEST_ASSERT_EQUAL_STRING("TE2", cs101->roster_turma[1]);

    // Roster indexes resolve to the registry, including bare-string entries.
    const student_t* maria = roster_student_at(cs101->roster[0]);
    TEST_ASSERT_NOT_NULL(maria);
    TEST_ASSERT_EQUAL_STRING("Maria Santos", maria->name);
    TEST_ASSERT_EQUAL_STRING("04:A3:1B:2C", maria->rfid_uid);

    const class_rec_t* ma110 = roster_class_at(1);
    TEST_ASSERT_NOT_NULL(ma110);
    TEST_ASSERT_EQUAL_STRING("Lucas Ferreira", roster_student_at(ma110->roster[0])->name);
    TEST_ASSERT_EQUAL_STRING("Maria Santos", roster_student_at(ma110->roster[1])->name);
    // Bare-string roster entries carry no turma.
    TEST_ASSERT_EQUAL_STRING("", ma110->roster_turma[0]);

    // Out of range.
    TEST_ASSERT_NULL(roster_class_at(2));
    TEST_ASSERT_NULL(roster_class_at(-1));
    TEST_ASSERT_NULL(roster_student_at(99));
}

static void test_teacher_filter(void) {
    const class_rec_t* cs101 = roster_class_at(0);
    const class_rec_t* ma110 = roster_class_at(1);
    TEST_ASSERT_NOT_NULL(cs101);
    TEST_ASSERT_NOT_NULL(ma110);

    TEST_ASSERT_TRUE(roster_class_matches_teacher(cs101, "h@x.edu"));
    TEST_ASSERT_TRUE(roster_class_matches_teacher(cs101, "H@X.EDU"));  // case-blind
    TEST_ASSERT_FALSE(roster_class_matches_teacher(cs101, "other@x.edu"));
    TEST_ASSERT_TRUE(roster_class_matches_teacher(ma110, "other@x.edu"));

    // Staff login (no email) sees everything.
    TEST_ASSERT_TRUE(roster_class_matches_teacher(cs101, ""));
    TEST_ASSERT_TRUE(roster_class_matches_teacher(ma110, nullptr));
}

// --- enrollment writes ------------------------------------------------------

static void setup_valid(void) {
    mocksd_reset();
    mocksd_add_file("/students/students.json", STUDENTS_JSON);
    mocksd_add_file("/classes/CS101-M1/class.json", CLASS_CS101);
    roster_service_start();
    TEST_ASSERT_EQUAL(ROSTER_OK, roster_get_status());
}

static int find_idx(const char* id) {
    for (int i = 0; i < roster_student_count(); i++) {
        if (strcmp(roster_student_at(i)->id, id) == 0) return i;
    }
    return -1;
}

static void read_card(const char* path, char* buf, size_t cap) {
    size_t n = mocksd_read_file(path, buf, cap - 1);
    buf[n] = '\0';
}

static void test_enroll_existing_binds_and_persists(void) {
    setup_valid();
    int idx = find_idx("2023-0187");  // John Miller, no card yet
    TEST_ASSERT_TRUE(idx >= 0);
    TEST_ASSERT_EQUAL_STRING("", roster_student_at(idx)->rfid_uid);

    roster_result_t r = roster_enroll_existing("CS101-M1", idx, "AA:BB:CC:01");
    TEST_ASSERT_TRUE(r.ok);
    TEST_ASSERT_NOT_NULL(strstr(r.message, "John Miller"));
    // In-RAM reflects the binding.
    TEST_ASSERT_EQUAL_STRING("AA:BB:CC:01", roster_student_at(idx)->rfid_uid);
    // students.json on the card was rewritten with the UID.
    char buf[2048];
    read_card("/students/students.json", buf, sizeof(buf));
    TEST_ASSERT_NOT_NULL(strstr(buf, "AA:BB:CC:01"));
    // The temp file must not linger.
    TEST_ASSERT_FALSE(mocksd_exists("/students/students.json.tmp"));
}

static void test_enroll_existing_rejects_used_card(void) {
    setup_valid();
    int john = find_idx("2023-0187");
    // Maria (2023-0142) already owns 04:A3:1B:2C.
    roster_result_t r = roster_enroll_existing("CS101-M1", john, "04-a3-1b-2c");
    TEST_ASSERT_FALSE(r.ok);
    TEST_ASSERT_NOT_NULL(strstr(r.message, "already assigned"));
    TEST_ASSERT_NOT_NULL(strstr(r.message, "Maria Santos"));  // names the owner
}

static void test_enroll_existing_adds_to_class_roster(void) {
    setup_valid();
    int lucas = find_idx("2024-0021");  // not in CS101's roster
    int ci = roster_class_index("CS101-M1");
    int before = roster_class_at(ci)->roster_count;

    roster_result_t r = roster_enroll_existing("CS101-M1", lucas, "77:88:99:AA");
    TEST_ASSERT_TRUE(r.ok);
    TEST_ASSERT_EQUAL_INT(before + 1, roster_class_at(ci)->roster_count);
    char buf[2048];
    read_card("/classes/CS101-M1/class.json", buf, sizeof(buf));
    TEST_ASSERT_NOT_NULL(strstr(buf, "2024-0021"));
}

static void test_enroll_new_adds_student_and_enrolls(void) {
    setup_valid();
    int before_students = roster_student_count();
    int ci = roster_class_index("CS101-M1");
    int before_roster = roster_class_at(ci)->roster_count;

    roster_result_t r =
        roster_enroll_new("CS101-M1", "2025-0500", "New Student", "DD:EE:FF:02", "TE3");
    TEST_ASSERT_TRUE(r.ok);
    TEST_ASSERT_EQUAL_INT(before_students + 1, roster_student_count());
    TEST_ASSERT_EQUAL_INT(before_roster + 1, roster_class_at(ci)->roster_count);

    char buf[2048];
    read_card("/students/students.json", buf, sizeof(buf));
    TEST_ASSERT_NOT_NULL(strstr(buf, "2025-0500"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "New Student"));
    read_card("/classes/CS101-M1/class.json", buf, sizeof(buf));
    TEST_ASSERT_NOT_NULL(strstr(buf, "2025-0500"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "TE3"));  // turma written onto the roster entry

    // The turma persists across a reload and doesn't leak into the registry.
    roster_service_start();
    read_card("/students/students.json", buf, sizeof(buf));
    TEST_ASSERT_NULL(strstr(buf, "TE3"));
}

static void test_enroll_new_rejects_overlong_turma(void) {
    setup_valid();
    roster_result_t r = roster_enroll_new("CS101-M1", "2025-0777", "Long Turma", "DD:EE:FF:77",
                                          "THIS-TURMA-IS-WAY-TOO-LONG");
    TEST_ASSERT_FALSE(r.ok);
    TEST_ASSERT_NOT_NULL(strstr(r.message, "Turma"));
}

static void test_enroll_new_rejects_existing_id(void) {
    setup_valid();
    roster_result_t r = roster_enroll_new("CS101-M1", "2023-0142", "Dup", "99:99:99:99", nullptr);
    TEST_ASSERT_FALSE(r.ok);
    TEST_ASSERT_NOT_NULL(strstr(r.message, "already exists"));
}

// A student must never get a card that belongs to a professor. Loads a config
// with a teacher card and confirms both enroll paths reject it (formatting
// differences included). Runs last: it leaves config populated.
static void test_enroll_rejects_professor_card(void) {
    mocksd_reset();
    mocksd_add_file("/students/students.json", STUDENTS_JSON);
    mocksd_add_file("/classes/CS101-M1/class.json", CLASS_CS101);
    mocksd_add_file("/config.json",
                    "{ \"teachers\": [ { \"name\": \"Prof X\", \"rfid_uid\": \"AB:CD:EF:01\",\n"
                    "  \"password\": \"1234\" } ] }");
    config_service_start();
    roster_service_start();
    TEST_ASSERT_EQUAL(ROSTER_OK, roster_get_status());

    roster_result_t r = roster_enroll_new("CS101-M1", "2025-0999", "Nope", "ab-cd-ef-01", nullptr);
    TEST_ASSERT_FALSE(r.ok);
    TEST_ASSERT_NOT_NULL(strstr(r.message, "Prof X"));

    int idx = find_idx("2023-0187");
    r = roster_enroll_existing("CS101-M1", idx, "AB:CD:EF:01");
    TEST_ASSERT_FALSE(r.ok);
    TEST_ASSERT_NOT_NULL(strstr(r.message, "Prof X"));
}

static void test_clear_all_uids(void) {
    setup_valid();
    // Maria Santos starts bound to a card.
    int maria = find_idx("2023-0142");
    TEST_ASSERT_TRUE(maria >= 0);
    TEST_ASSERT_EQUAL_STRING("04:A3:1B:2C", roster_student_at(maria)->rfid_uid);

    TEST_ASSERT_TRUE(roster_clear_all_uids());

    // In-RAM: every student is now unbound, but still enrolled.
    for (int i = 0; i < roster_student_count(); i++) {
        TEST_ASSERT_EQUAL_STRING("", roster_student_at(i)->rfid_uid);
    }
    // Persisted: students.json no longer carries the UID, and a reload confirms.
    char buf[512];
    read_card("/students/students.json", buf, sizeof(buf));
    TEST_ASSERT_NULL(strstr(buf, "04:A3:1B:2C"));
    roster_service_start();
    TEST_ASSERT_EQUAL_STRING("", roster_student_at(find_idx("2023-0142"))->rfid_uid);
}

// --- roster_validate_tree (import: validate a staged tree, no live mutation) -

static void test_validate_tree_accepts_good_staging(void) {
    setup_valid();  // live roster OK (3 students, 1 class)
    mocksd_add_file("/import_staging/students/students.json", STUDENTS_JSON);
    mocksd_add_file("/import_staging/classes/CS101-M1/class.json", CLASS_CS101);

    char msg[160] = "dirty";
    TEST_ASSERT_TRUE(roster_validate_tree("/import_staging", msg, sizeof(msg)));
    TEST_ASSERT_EQUAL_STRING("", msg);
    // Live tree was restored: still OK, still the three students.
    TEST_ASSERT_EQUAL(ROSTER_OK, roster_get_status());
    TEST_ASSERT_EQUAL(3, roster_student_count());
}

static void test_validate_tree_rejects_bad_staging_leaving_live_intact(void) {
    // A staged class references a student absent from the staged registry.
    mocksd_add_file("/import_staging/students/students.json", STUDENTS_JSON);
    mocksd_add_file(
        "/import_staging/classes/BAD/class.json",
        "{ \"version\": 1, \"code\": \"BAD\", \"name\": \"X\", "
        "\"roster\": [ { \"id\": \"9999-9999\" } ] }");

    char msg[160];
    TEST_ASSERT_FALSE(roster_validate_tree("/import_staging", msg, sizeof(msg)));
    TEST_ASSERT_NOT_NULL(strstr(msg, "unknown student"));

    // Live tree is intact and re-readable, and its error string is cleared.
    TEST_ASSERT_EQUAL(ROSTER_OK, roster_get_status());
    TEST_ASSERT_EQUAL(3, roster_student_count());
    TEST_ASSERT_TRUE(find_idx("2023-0142") >= 0);
    char err[160];
    roster_get_error(err, sizeof(err));
    TEST_ASSERT_EQUAL_STRING("", err);
}

int main(int, char**) {
    mock_freertos_set_delay_scale(1000);
    event_bus_init();
    UNITY_BEGIN();
    RUN_TEST(test_no_sd_card);  // must stay first (mount latches)
    RUN_TEST(test_missing_students_file);
    RUN_TEST(test_bad_students_json_syntax);
    RUN_TEST(test_student_missing_name);
    RUN_TEST(test_duplicate_student_id);
    RUN_TEST(test_duplicate_uid_across_formats);
    RUN_TEST(test_class_with_unknown_student);
    RUN_TEST(test_class_student_twice_with_different_turmas);
    RUN_TEST(test_class_dir_without_class_json);
    RUN_TEST(test_no_classes_dir_is_ok);
    RUN_TEST(test_valid_full_layout);
    RUN_TEST(test_accessors_expose_loaded_data);  // depends on the previous test
    RUN_TEST(test_teacher_filter);
    RUN_TEST(test_enroll_existing_binds_and_persists);
    RUN_TEST(test_enroll_existing_rejects_used_card);
    RUN_TEST(test_enroll_existing_adds_to_class_roster);
    RUN_TEST(test_enroll_new_adds_student_and_enrolls);
    RUN_TEST(test_enroll_new_rejects_existing_id);
    RUN_TEST(test_enroll_new_rejects_overlong_turma);
    RUN_TEST(test_clear_all_uids);
    RUN_TEST(test_enroll_rejects_professor_card);  // last of the enroll chain
    RUN_TEST(test_validate_tree_accepts_good_staging);          // resets to a fresh valid live tree
    RUN_TEST(test_validate_tree_rejects_bad_staging_leaving_live_intact);  // chains from above
    return UNITY_END();
}
