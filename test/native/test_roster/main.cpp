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
#include "app/credential.h"
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
    "  \"schedule\": \"Tue/Thu 10:00-12:00\", \"teacher_emails\": [\"h@x.edu\"],\n"
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

// A class the loader can't resolve is SKIPPED, not fatal: one broken folder
// (typically left over from a previous config, since import is an overlay) must
// not blank the whole class list. The skip is counted + explained for the UI.
static void test_class_with_unknown_student_is_skipped_not_fatal(void) {
    mocksd_reset();
    mocksd_add_file("/students/students.json", STUDENTS_JSON);
    mocksd_add_file("/classes/CS101-M1/class.json",
                    "{ \"code\": \"CS101-M1\", \"name\": \"DS\",\n"
                    "  \"roster\": [ { \"id\": \"9999-9999\" } ] }");
    roster_service_start();
    TEST_ASSERT_EQUAL(ROSTER_OK, roster_get_status());
    TEST_ASSERT_EQUAL_INT(0, roster_class_count());
    TEST_ASSERT_EQUAL_INT(1, roster_skipped_class_count());

    char why[160];
    roster_get_skip_reason(why, sizeof(why));
    TEST_ASSERT_NOT_NULL(strstr(why, "CS101-M1"));
    TEST_ASSERT_NOT_NULL(strstr(why, "unknown student 9999-9999"));
}

// The good classes still load alongside a broken one — the whole point.
static void test_good_classes_survive_a_broken_sibling(void) {
    mocksd_reset();
    mocksd_add_file("/students/students.json", STUDENTS_JSON);
    mocksd_add_file("/classes/BROKEN/class.json",
                    "{ \"code\": \"BROKEN\", \"name\": \"Stale\",\n"
                    "  \"roster\": [ { \"id\": \"9999-9999\" } ] }");
    mocksd_add_file("/classes/GOOD/class.json",
                    "{ \"code\": \"GOOD\", \"name\": \"Fine\",\n"
                    "  \"roster\": [ { \"id\": \"2023-0142\" } ] }");
    roster_service_start();
    TEST_ASSERT_EQUAL(ROSTER_OK, roster_get_status());
    TEST_ASSERT_EQUAL_INT(1, roster_class_count());
    TEST_ASSERT_EQUAL_STRING("GOOD", roster_class_at(0)->code);
    TEST_ASSERT_EQUAL_INT(1, roster_skipped_class_count());
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
    TEST_ASSERT_EQUAL(ROSTER_OK, roster_get_status());  // skipped, not fatal
    TEST_ASSERT_EQUAL_INT(1, roster_skipped_class_count());
    char why[160];
    roster_get_skip_reason(why, sizeof(why));
    TEST_ASSERT_NOT_NULL(strstr(why, "CS101-M1"));
    TEST_ASSERT_NOT_NULL(strstr(why, "listed twice"));
}

static void test_class_dir_without_class_json(void) {
    mocksd_reset();
    mocksd_add_file("/students/students.json", STUDENTS_JSON);
    mocksd_add_dir("/classes/CS205-T2");
    roster_service_start();
    // An orphan folder (e.g. attendance left behind) is skipped, not fatal.
    TEST_ASSERT_EQUAL(ROSTER_OK, roster_get_status());
    TEST_ASSERT_EQUAL_INT(1, roster_skipped_class_count());
    char why[160];
    roster_get_skip_reason(why, sizeof(why));
    TEST_ASSERT_NOT_NULL(strstr(why, "CS205-T2"));
    TEST_ASSERT_NOT_NULL(strstr(why, "class.json is missing"));
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
                    "  \"teacher_emails\": [\"other@x.edu\"],\n"
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
    TEST_ASSERT_EQUAL_INT(1, cs101->teacher_count);
    TEST_ASSERT_EQUAL_STRING("h@x.edu", cs101->teacher_emails[0]);
    TEST_ASSERT_EQUAL_HEX32(0x272766, cs101->color);
    // A roster of {"id","turma"} objects loads fine, turma tags into RAM.
    TEST_ASSERT_EQUAL_INT(2, cs101->roster_count);
    TEST_ASSERT_EQUAL_STRING("TE1", cs101->roster_turma[0]);
    TEST_ASSERT_EQUAL_STRING("TE2", cs101->roster_turma[1]);

    // Roster indexes resolve to the registry, including bare-string entries.
    const student_t* maria = roster_student_at(cs101->roster[0]);
    TEST_ASSERT_NOT_NULL(maria);
    TEST_ASSERT_EQUAL_STRING("Maria Santos", maria->name);
    // The authored card id was converted on load: stored as a keyed fingerprint,
    // never the id itself. That it still resolves her card is asserted in
    // test_authored_card_is_converted_and_still_matches below.
    TEST_ASSERT_TRUE(credential_is_fingerprint(maria->rfid_uid, UID_FINGERPRINT_HEX));

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
    // In-RAM reflects the binding, as a fingerprint rather than the card id.
    TEST_ASSERT_TRUE(credential_is_fingerprint(roster_student_at(idx)->rfid_uid,
                                               UID_FINGERPRINT_HEX));
    // students.json was rewritten — and must NOT contain the card id anywhere.
    // Enrolling is where a card id enters the device, so this is the assertion
    // that would catch it being written through in the clear.
    char buf[2048];
    read_card("/students/students.json", buf, sizeof(buf));
    TEST_ASSERT_NULL(strstr(buf, "AA:BB:CC:01"));
    TEST_ASSERT_NOT_NULL(strstr(buf, roster_student_at(idx)->rfid_uid));
    // ...and the card still resolves to this student on a re-scan.
    char owner[48];
    TEST_ASSERT_TRUE(roster_uid_belongs_to_student("aa-bb-cc-01", owner, sizeof(owner)));
    TEST_ASSERT_EQUAL_STRING("John Miller", owner);
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

// --- registry writes with no card (the student registry screen) -------------

static void test_class_add_existing_without_card(void) {
    setup_valid();
    int lucas = find_idx("2024-0021");  // in the registry, not in CS101's roster
    TEST_ASSERT_TRUE(lucas >= 0);
    int ci = roster_class_index("CS101-M1");
    int before = roster_class_at(ci)->roster_count;
    char students_before[2048];
    read_card("/students/students.json", students_before, sizeof(students_before));

    roster_result_t r = roster_class_add_existing("CS101-M1", lucas, nullptr);
    TEST_ASSERT_TRUE(r.ok);
    TEST_ASSERT_NOT_NULL(strstr(r.message, "Lucas Ferreira"));
    TEST_ASSERT_EQUAL_INT(before + 1, roster_class_at(ci)->roster_count);
    // The card is untouched: still unbound, and students.json is byte-identical.
    TEST_ASSERT_EQUAL_STRING("", roster_student_at(lucas)->rfid_uid);
    char students_after[2048];
    read_card("/students/students.json", students_after, sizeof(students_after));
    TEST_ASSERT_EQUAL_STRING(students_before, students_after);

    char buf[2048];
    read_card("/classes/CS101-M1/class.json", buf, sizeof(buf));
    TEST_ASSERT_NOT_NULL(strstr(buf, "2024-0021"));
}

static void test_class_add_existing_twice_is_a_noop(void) {
    setup_valid();
    int maria = find_idx("2023-0142");  // already in CS101's roster
    int ci = roster_class_index("CS101-M1");
    int before = roster_class_at(ci)->roster_count;

    roster_result_t r = roster_class_add_existing("CS101-M1", maria, nullptr);
    TEST_ASSERT_TRUE(r.ok);  // success, but it says nothing was added
    TEST_ASSERT_NOT_NULL(strstr(r.message, "already in this class"));
    TEST_ASSERT_EQUAL_INT(before, roster_class_at(ci)->roster_count);

    // And the id appears exactly once in the file.
    char buf[2048];
    read_card("/classes/CS101-M1/class.json", buf, sizeof(buf));
    const char* first = strstr(buf, "2023-0142");
    TEST_ASSERT_NOT_NULL(first);
    TEST_ASSERT_NULL(strstr(first + 1, "2023-0142"));
}

// The point of the no-card path: the student is stored exactly like an imported
// one ("rfid_uid": null) and their card binds itself on the first tap.
static void test_class_add_new_without_card_binds_on_first_tap(void) {
    setup_valid();
    int before_students = roster_student_count();
    int ci = roster_class_index("CS101-M1");
    int before_roster = roster_class_at(ci)->roster_count;

    roster_result_t r = roster_class_add_new("CS101-M1", "2025-0601", "Ana Prado", "TE4");
    TEST_ASSERT_TRUE(r.ok);
    TEST_ASSERT_EQUAL_INT(before_students + 1, roster_student_count());
    TEST_ASSERT_EQUAL_INT(before_roster + 1, roster_class_at(ci)->roster_count);

    char buf[2048];
    read_card("/students/students.json", buf, sizeof(buf));
    TEST_ASSERT_NOT_NULL(strstr(buf, "2025-0601"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"rfid_uid\": null"));
    read_card("/classes/CS101-M1/class.json", buf, sizeof(buf));
    TEST_ASSERT_NOT_NULL(strstr(buf, "TE4"));

    // Survives a reload as an unbound student...
    roster_service_start();
    TEST_ASSERT_EQUAL(ROSTER_OK, roster_get_status());
    int idx = find_idx("2025-0601");
    TEST_ASSERT_TRUE(idx >= 0);
    TEST_ASSERT_EQUAL_STRING("", roster_student_at(idx)->rfid_uid);

    // ...and the first tap binds the card to them.
    TEST_ASSERT_TRUE(roster_enroll_existing("CS101-M1", idx, "11:22:33:44").ok);
    char owner[48];
    TEST_ASSERT_TRUE(roster_uid_belongs_to_student("11-22-33-44", owner, sizeof(owner)));
    TEST_ASSERT_EQUAL_STRING("Ana Prado", owner);
}

static void test_class_add_new_rejects_bad_fields(void) {
    setup_valid();
    int before = roster_student_count();

    roster_result_t r = roster_class_add_new("CS101-M1", "2023-0142", "Dup", nullptr);
    TEST_ASSERT_FALSE(r.ok);
    TEST_ASSERT_NOT_NULL(strstr(r.message, "already exists"));

    r = roster_class_add_new("CS101-M1", "", "No Id", nullptr);
    TEST_ASSERT_FALSE(r.ok);
    TEST_ASSERT_NOT_NULL(strstr(r.message, "required"));

    // Over-length is REJECTED, not truncated: a truncated id is a different
    // student, silently.
    r = roster_class_add_new("CS101-M1", "2025-0000-0000-0000-0000", "Long Id", nullptr);
    TEST_ASSERT_FALSE(r.ok);
    TEST_ASSERT_NOT_NULL(strstr(r.message, "ID too long"));

    char long_name[80];
    memset(long_name, 'A', sizeof(long_name) - 1);
    long_name[sizeof(long_name) - 1] = '\0';
    r = roster_class_add_new("CS101-M1", "2025-0602", long_name, nullptr);
    TEST_ASSERT_FALSE(r.ok);
    TEST_ASSERT_NOT_NULL(strstr(r.message, "Name too long"));

    r = roster_class_add_new("CS101-M1", "2025-0603", "Long Turma", "THIS-TURMA-IS-TOO-LONG");
    TEST_ASSERT_FALSE(r.ok);
    TEST_ASSERT_NOT_NULL(strstr(r.message, "Turma"));

    r = roster_class_add_new("NOPE-M9", "2025-0604", "No Class", nullptr);
    TEST_ASSERT_FALSE(r.ok);
    TEST_ASSERT_NOT_NULL(strstr(r.message, "Unknown class"));

    TEST_ASSERT_EQUAL_INT(before, roster_student_count());  // nothing was written
}

// A full class must be refused by EVERY add path. persist_enroll() would
// otherwise append to class.json while ram_enroll() dropped the entry, and the
// next load would reject the oversized roster and skip the whole class.
static void test_add_to_full_class_is_refused(void) {
    char students[40960];
    char cls[40960];
    int sn = snprintf(students, sizeof(students), "{ \"version\": 1, \"students\": [");
    int cn = snprintf(cls, sizeof(cls),
                      "{ \"version\": 1, \"code\": \"FULL-M1\", \"name\": \"Full\","
                      " \"teacher_emails\": [\"h@x.edu\"], \"roster\": [");
    for (int i = 0; i < ROSTER_MAX_CLASS_STUDENTS; i++) {
        sn += snprintf(students + sn, sizeof(students) - sn,
                       "%s{ \"id\": \"F%03d\", \"name\": \"Full Student %03d\" }", i ? "," : "", i,
                       i);
        cn += snprintf(cls + cn, sizeof(cls) - cn, "%s{ \"id\": \"F%03d\" }", i ? "," : "", i);
    }
    // One spare student in the registry who is NOT in the class.
    snprintf(students + sn, sizeof(students) - sn,
             ", { \"id\": \"SPARE\", \"name\": \"Spare One\" } ] }");
    snprintf(cls + cn, sizeof(cls) - cn, "] }");

    mocksd_reset();
    mocksd_add_file("/students/students.json", students);
    mocksd_add_file("/classes/FULL-M1/class.json", cls);
    roster_service_start();
    TEST_ASSERT_EQUAL(ROSTER_OK, roster_get_status());
    int ci = roster_class_index("FULL-M1");
    TEST_ASSERT_EQUAL_INT(ROSTER_MAX_CLASS_STUDENTS, roster_class_at(ci)->roster_count);

    char before[40960];
    read_card("/classes/FULL-M1/class.json", before, sizeof(before));

    int spare = find_idx("SPARE");
    roster_result_t r = roster_class_add_existing("FULL-M1", spare, nullptr);
    TEST_ASSERT_FALSE(r.ok);
    TEST_ASSERT_NOT_NULL(strstr(r.message, "full"));

    r = roster_class_add_new("FULL-M1", "F999", "Too Many", nullptr);
    TEST_ASSERT_FALSE(r.ok);
    TEST_ASSERT_NOT_NULL(strstr(r.message, "full"));

    // The card-tap paths are gated by the same guard.
    r = roster_enroll_existing("FULL-M1", spare, "AA:BB:CC:99");
    TEST_ASSERT_FALSE(r.ok);
    TEST_ASSERT_NOT_NULL(strstr(r.message, "full"));

    r = roster_enroll_new("FULL-M1", "F998", "Too Many Too", "AA:BB:CC:98", nullptr);
    TEST_ASSERT_FALSE(r.ok);
    TEST_ASSERT_NOT_NULL(strstr(r.message, "full"));

    // class.json is untouched, so the class still loads.
    char after[40960];
    read_card("/classes/FULL-M1/class.json", after, sizeof(after));
    TEST_ASSERT_EQUAL_STRING(before, after);
    TEST_ASSERT_NULL(strstr(after, "SPARE"));
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
    TEST_ASSERT_TRUE(credential_is_fingerprint(roster_student_at(maria)->rfid_uid,
                                               UID_FINGERPRINT_HEX));

    roster_result_t cleared = roster_clear_all_uids();
    TEST_ASSERT_TRUE_MESSAGE(cleared.ok, cleared.message);
    // The message is shown verbatim in the toast, so it must say something.
    TEST_ASSERT_TRUE(cleared.message[0] != '\0');

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

// A failed wipe must explain WHY — the old bare-bool version could only ever
// surface "Card clear failed", leaving nothing to debug from.
static void test_clear_all_uids_reports_why_it_failed(void) {
    mocksd_reset();  // no students.json at all -> roster never loads
    roster_service_start();
    TEST_ASSERT_NOT_EQUAL(ROSTER_OK, roster_get_status());

    roster_result_t r = roster_clear_all_uids();
    TEST_ASSERT_FALSE(r.ok);
    TEST_ASSERT_TRUE(r.message[0] != '\0');
    // Names the actual problem rather than a generic failure.
    TEST_ASSERT_NOT_NULL(strstr(r.message, "not loaded"));
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

// --- per-class settings (capture / face-verify / metadata editing) ----------

// --- multi-professor classes (CONFIG_IMPORT.md §3.3 "teacher_emails") -------

static void test_class_with_several_teachers(void) {
    mocksd_reset();
    mocksd_add_file("/students/students.json", STUDENTS_JSON);
    mocksd_add_file("/classes/CS101-M1/class.json",
                    "{ \"version\": 1, \"code\": \"CS101-M1\", \"name\": \"DS\", "
                    "\"teacher_emails\": [ \"a@x.edu\", \"b@x.edu\", \"c@x.edu\" ], "
                    "\"roster\": [ { \"id\": \"2023-0142\" } ] }");
    roster_service_start();
    TEST_ASSERT_EQUAL(ROSTER_OK, roster_get_status());

    const class_rec_t* c = roster_class_at(0);
    TEST_ASSERT_EQUAL_INT(3, c->teacher_count);
    TEST_ASSERT_EQUAL_STRING("a@x.edu", c->teacher_emails[0]);
    TEST_ASSERT_EQUAL_STRING("c@x.edu", c->teacher_emails[2]);

    // The class lists for ANY of its professors, case-insensitively...
    TEST_ASSERT_TRUE(roster_class_matches_teacher(c, "a@x.edu"));
    TEST_ASSERT_TRUE(roster_class_matches_teacher(c, "B@X.edu"));
    TEST_ASSERT_TRUE(roster_class_matches_teacher(c, "c@x.edu"));
    // ...and not for anyone else. An empty email (staff login) sees everything.
    TEST_ASSERT_FALSE(roster_class_matches_teacher(c, "ghost@x.edu"));
    TEST_ASSERT_FALSE(roster_class_matches_teacher(c, "a@x.ed"));   // prefix, not a match
    TEST_ASSERT_FALSE(roster_class_matches_teacher(c, "a@x.edu.br"));  // longer, not a match
    TEST_ASSERT_TRUE(roster_class_matches_teacher(c, ""));
}

// Cards written before multi-professor support carry a scalar "teacher_email".
static void test_legacy_scalar_teacher_email_still_loads(void) {
    mocksd_reset();
    mocksd_add_file("/students/students.json", STUDENTS_JSON);
    mocksd_add_file("/classes/CS101-M1/class.json",
                    "{ \"version\": 1, \"code\": \"CS101-M1\", \"name\": \"DS\", "
                    "\"teacher_email\": \"legacy@x.edu\", "
                    "\"roster\": [ { \"id\": \"2023-0142\" } ] }");
    roster_service_start();
    TEST_ASSERT_EQUAL(ROSTER_OK, roster_get_status());

    const class_rec_t* c = roster_class_at(0);
    TEST_ASSERT_EQUAL_INT(1, c->teacher_count);
    TEST_ASSERT_EQUAL_STRING("legacy@x.edu", c->teacher_emails[0]);
    TEST_ASSERT_TRUE(roster_class_matches_teacher(c, "legacy@x.edu"));
}

// A class with no professor loads (the device only warns) but lists for nobody.
static void test_class_without_teachers_loads_but_matches_nobody(void) {
    mocksd_reset();
    mocksd_add_file("/students/students.json", STUDENTS_JSON);
    mocksd_add_file("/classes/CS101-M1/class.json",
                    "{ \"version\": 1, \"code\": \"CS101-M1\", \"name\": \"DS\", "
                    "\"teacher_emails\": [], \"roster\": [ { \"id\": \"2023-0142\" } ] }");
    roster_service_start();
    TEST_ASSERT_EQUAL(ROSTER_OK, roster_get_status());

    const class_rec_t* c = roster_class_at(0);
    TEST_ASSERT_EQUAL_INT(0, c->teacher_count);
    TEST_ASSERT_FALSE(roster_class_matches_teacher(c, "anyone@x.edu"));
    TEST_ASSERT_TRUE(roster_class_matches_teacher(c, ""));  // staff still sees it
}

// More professors than the cap: extras are dropped, the class still loads.
static void test_teacher_list_is_capped(void) {
    mocksd_reset();
    mocksd_add_file("/students/students.json", STUDENTS_JSON);
    char json[512];
    int n = snprintf(json, sizeof(json),
                     "{ \"version\": 1, \"code\": \"CS101-M1\", \"name\": \"DS\", "
                     "\"teacher_emails\": [");
    for (int i = 0; i < ROSTER_MAX_CLASS_TEACHERS + 3; i++) {
        n += snprintf(json + n, sizeof(json) - n, "%s\"t%d@x.edu\"", i ? ", " : "", i);
    }
    snprintf(json + n, sizeof(json) - n, "], \"roster\": [ { \"id\": \"2023-0142\" } ] }");
    mocksd_add_file("/classes/CS101-M1/class.json", json);
    roster_service_start();
    TEST_ASSERT_EQUAL(ROSTER_OK, roster_get_status());
    TEST_ASSERT_EQUAL_INT(ROSTER_MAX_CLASS_TEACHERS, roster_class_at(0)->teacher_count);
}

// Blank entries are skipped rather than becoming empty professors.
static void test_blank_teacher_entries_are_skipped(void) {
    mocksd_reset();
    mocksd_add_file("/students/students.json", STUDENTS_JSON);
    mocksd_add_file("/classes/CS101-M1/class.json",
                    "{ \"version\": 1, \"code\": \"CS101-M1\", \"name\": \"DS\", "
                    "\"teacher_emails\": [ \"\", \"real@x.edu\", \"\" ], "
                    "\"roster\": [ { \"id\": \"2023-0142\" } ] }");
    roster_service_start();
    TEST_ASSERT_EQUAL(ROSTER_OK, roster_get_status());
    const class_rec_t* c = roster_class_at(0);
    TEST_ASSERT_EQUAL_INT(1, c->teacher_count);
    TEST_ASSERT_EQUAL_STRING("real@x.edu", c->teacher_emails[0]);
}

static void test_class_capture_and_settings(void) {
    mocksd_reset();
    mocksd_add_file("/students/students.json", STUDENTS_JSON);
    mocksd_add_file("/classes/CS101-M1/class.json",
                    "{ \"version\": 1, \"code\": \"CS101-M1\", \"name\": \"DS\", "
                    "\"schedule\": \"Tue\", \"color\": \"272766\", \"capture_photos\": true, "
                    "\"face_verify_seconds\": 20, "
                    "\"roster\": [ { \"id\": \"2023-0142\", \"turma\": \"TE1\" } ] }");
    roster_service_start();
    TEST_ASSERT_EQUAL(ROSTER_OK, roster_get_status());
    TEST_ASSERT_TRUE(roster_class_at(0)->capture_photos);
    TEST_ASSERT_EQUAL_INT(20, roster_class_at(0)->face_verify_seconds);
    TEST_ASSERT_TRUE(class_capture_enabled(roster_class_at(0)));

    // Rename + new schedule/color, turn capture OFF, face-verify clamps (99->60),
    // enable timed (60 min).
    roster_result_t r = roster_class_update_settings("CS101-M1", "Data Structures II", "Wed 8h",
                                                     0xABCDEF, false, 99, true, 60);
    TEST_ASSERT_TRUE(r.ok);
    TEST_ASSERT_EQUAL_STRING("Data Structures II", roster_class_at(0)->name);
    TEST_ASSERT_EQUAL_STRING("Wed 8h", roster_class_at(0)->schedule);
    TEST_ASSERT_EQUAL_HEX32(0xABCDEF, roster_class_at(0)->color);
    TEST_ASSERT_FALSE(roster_class_at(0)->capture_photos);
    TEST_ASSERT_FALSE(class_capture_enabled(roster_class_at(0)));
    TEST_ASSERT_EQUAL_INT(FACE_VERIFY_SECONDS_MAX, roster_class_at(0)->face_verify_seconds);
    TEST_ASSERT_TRUE(roster_class_at(0)->timed_attendance);
    TEST_ASSERT_EQUAL_INT(60, roster_class_at(0)->min_attendance_min);

    // Persisted, and the roster + turma survived the rewrite.
    char buf[512];
    read_card("/classes/CS101-M1/class.json", buf, sizeof(buf));
    TEST_ASSERT_NOT_NULL(strstr(buf, "Data Structures II"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "ABCDEF"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "2023-0142"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "TE1"));
    roster_service_start();
    TEST_ASSERT_FALSE(roster_class_at(0)->capture_photos);
    TEST_ASSERT_EQUAL_INT(FACE_VERIFY_SECONDS_MAX, roster_class_at(0)->face_verify_seconds);
    TEST_ASSERT_EQUAL_STRING("TE1", roster_class_at(0)->roster_turma[0]);
    TEST_ASSERT_TRUE(roster_class_at(0)->timed_attendance);
    TEST_ASSERT_EQUAL_INT(60, roster_class_at(0)->min_attendance_min);
}

// --- authoring plaintext -> stored fingerprints ------------------------------

// A card authored in students.json must be converted on load AND still resolve
// on a tap. Converting rather than rejecting is what keeps an imported card
// working instead of silently unbinding every student.
static void test_authored_card_is_converted_and_still_matches(void) {
    setup_valid();  // Maria is authored with rfid_uid "04:A3:1B:2C"

    char buf[2048];
    read_card("/students/students.json", buf, sizeof(buf));
    TEST_ASSERT_NULL_MESSAGE(strstr(buf, "04:A3:1B:2C"), "card id left in the clear");
    TEST_ASSERT_NOT_NULL(strstr(buf, "v1:"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "Maria Santos"));  // the rest is untouched

    // The physical card still resolves, in whatever format the reader reports.
    char owner[48];
    TEST_ASSERT_TRUE(roster_uid_belongs_to_student("04:A3:1B:2C", owner, sizeof(owner)));
    TEST_ASSERT_EQUAL_STRING("Maria Santos", owner);
    TEST_ASSERT_TRUE(roster_uid_belongs_to_student("04a31b2c", owner, sizeof(owner)));
    TEST_ASSERT_EQUAL_STRING("Maria Santos", owner);
    TEST_ASSERT_FALSE(roster_uid_belongs_to_student("DE:AD:BE:EF", owner, sizeof(owner)));
}

// Re-hashing on the next boot would unbind every student permanently.
static void test_student_conversion_is_idempotent(void) {
    setup_valid();
    char first[2048];
    read_card("/students/students.json", first, sizeof(first));

    roster_service_reload();
    roster_service_reload();
    TEST_ASSERT_EQUAL(ROSTER_OK, roster_get_status());

    char third[2048];
    read_card("/students/students.json", third, sizeof(third));
    TEST_ASSERT_EQUAL_STRING_MESSAGE(first, third,
                                     "students.json changed on reload - not idempotent");
    char owner[48];
    TEST_ASSERT_TRUE(roster_uid_belongs_to_student("04:A3:1B:2C", owner, sizeof(owner)));
}

// Both files are keyed by the same device secret, so the cross-file guard still
// works with neither side readable: a professor's card must not become a
// student's, in either direction.
static void test_professor_card_is_still_refused_for_a_student(void) {
    mocksd_reset();
    mocksd_add_file("/config.json",
                    "{ \"teachers\": [ { \"name\": \"Prof A\", \"email\": \"a@x\", "
                    "\"rfid_uid\": \"CA:FE:00:01\", \"password\": \"111111\" } ] }");
    mocksd_add_file("/students/students.json",
                    "{ \"students\": [ { \"id\": \"S1\", \"name\": \"Sam\" } ] }");
    mocksd_add_file("/classes/CS101-M1/class.json",
                    "{ \"code\": \"CS101-M1\", \"name\": \"DS\", "
                    "\"roster\": [ { \"id\": \"S1\" } ] }");
    config_service_start();
    roster_service_start();
    TEST_ASSERT_EQUAL(CONFIG_OK, config_get_status());
    TEST_ASSERT_EQUAL(ROSTER_OK, roster_get_status());

    // Enrolling the professor's card onto a student is refused, and names them.
    int s1 = find_idx("S1");
    TEST_ASSERT_TRUE(s1 >= 0);
    roster_result_t r = roster_enroll_existing("CS101-M1", s1, "ca-fe-00-01");
    TEST_ASSERT_FALSE(r.ok);
    TEST_ASSERT_NOT_NULL(strstr(r.message, "Prof A"));

    // A card belonging to nobody is still accepted.
    TEST_ASSERT_TRUE(roster_enroll_existing("CS101-M1", s1, "BE:EF:00:02").ok);
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
    RUN_TEST(test_class_with_unknown_student_is_skipped_not_fatal);
    RUN_TEST(test_good_classes_survive_a_broken_sibling);
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
    RUN_TEST(test_class_add_existing_without_card);
    RUN_TEST(test_class_add_existing_twice_is_a_noop);
    RUN_TEST(test_class_add_new_without_card_binds_on_first_tap);
    RUN_TEST(test_class_add_new_rejects_bad_fields);
    RUN_TEST(test_add_to_full_class_is_refused);  // self-contained (own fixture)
    RUN_TEST(test_clear_all_uids);
    RUN_TEST(test_clear_all_uids_reports_why_it_failed);
    RUN_TEST(test_authored_card_is_converted_and_still_matches);
    RUN_TEST(test_student_conversion_is_idempotent);
    RUN_TEST(test_professor_card_is_still_refused_for_a_student);
    RUN_TEST(test_enroll_rejects_professor_card);  // last of the enroll chain
    RUN_TEST(test_validate_tree_accepts_good_staging);          // resets to a fresh valid live tree
    RUN_TEST(test_validate_tree_rejects_bad_staging_leaving_live_intact);  // chains from above
    RUN_TEST(test_class_capture_and_settings);  // self-contained
    RUN_TEST(test_class_with_several_teachers);
    RUN_TEST(test_legacy_scalar_teacher_email_still_loads);
    RUN_TEST(test_class_without_teachers_loads_but_matches_nobody);
    RUN_TEST(test_teacher_list_is_capped);
    RUN_TEST(test_blank_teacher_entries_are_skipped);
    return UNITY_END();
}
