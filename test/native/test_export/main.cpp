// export_service against the in-memory SD card: per-class metrics and the
// MATRICULA,FREQ CSV (FREQ = absent session days per student), the overwrite
// path, and that exporting leaves an open roll-call session untouched.
//
// Ordering: the first test establishes the SD mount for the binary.

#include <stdio.h>
#include <string.h>
#include <unity.h>

#include "app/event_bus.h"
#include "mock_freertos.h"
#include "mock_sd.h"
#include "services/export_service.h"
#include "services/roster_service.h"
#include "storage/attendance_store.h"

void setUp(void) {
    app_event_t ev;
    while (event_bus_poll(&ev)) {
    }
}
void tearDown(void) { attendance_close(); }

static const char* CLASS_DIR = "CS101-M1";

static const char* STUDENTS_JSON =
    "{ \"version\": 1, \"students\": [\n"
    "  { \"id\": \"202500001\", \"name\": \"Ana\" },\n"
    "  { \"id\": \"202500002\", \"name\": \"Bruno\" },\n"
    "  { \"id\": \"202500003\", \"name\": \"Carla\" }\n"
    "] }";

static const char* CLASS_JSON =
    "{ \"version\": 1, \"code\": \"CS101-M1\", \"name\": \"Data Structures\",\n"
    "  \"schedule\": \"Tue/Thu 10:00-12:00\", \"teacher_emails\": [\"h@x.edu\"],\n"
    "  \"color\": \"272766\",\n"
    "  \"roster\": [ { \"id\": \"202500001\" }, { \"id\": \"202500002\" },\n"
    "               { \"id\": \"202500003\" } ] }";

// Opens `date` and marks the listed ids present, then closes.
static void mark_day(const char* date, const char* const* present, int n) {
    attendance_open(CLASS_DIR, date);
    for (int i = 0; i < n; i++) attendance_set(present[i], true);
    attendance_close();
}

// Loads the roster and lays down 3 sessions producing absences 2/1/0.
static const class_rec_t* setup_data(void) {
    mocksd_reset();
    mocksd_add_file("/students/students.json", STUDENTS_JSON);
    mocksd_add_file("/classes/CS101-M1/class.json", CLASS_JSON);
    roster_service_start();
    TEST_ASSERT_EQUAL(ROSTER_OK, roster_get_status());

    const char* all[] = {"202500001", "202500002", "202500003"};
    const char* bc[] = {"202500002", "202500003"};
    const char* c[] = {"202500003"};
    mark_day("2026-01-10", all, 3);  // everyone present
    mark_day("2026-01-12", bc, 2);   // 001 absent
    mark_day("2026-01-15", c, 1);    // 001 + 002 absent

    return roster_class_at(0);
}

static void read_file(const char* path, char* buf, size_t cap) {
    size_t n = mocksd_read_file(path, buf, cap - 1);
    buf[n] = '\0';
}

static void test_metrics(void) {
    const class_rec_t* cls = setup_data();
    export_metrics_t m = export_metrics(cls);
    TEST_ASSERT_EQUAL_INT(3, m.student_count);
    TEST_ASSERT_EQUAL_INT(3, m.day_count);
    TEST_ASSERT_EQUAL_STRING("2026-01-10", m.start_date);  // earliest
    TEST_ASSERT_EQUAL_STRING("2026-01-15", m.end_date);    // latest
}

static void test_metrics_no_attendance(void) {
    mocksd_reset();
    mocksd_add_file("/students/students.json", STUDENTS_JSON);
    mocksd_add_file("/classes/CS101-M1/class.json", CLASS_JSON);
    roster_service_start();
    export_metrics_t m = export_metrics(roster_class_at(0));
    TEST_ASSERT_EQUAL_INT(3, m.student_count);
    TEST_ASSERT_EQUAL_INT(0, m.day_count);
    TEST_ASSERT_EQUAL_STRING("", m.start_date);
    TEST_ASSERT_EQUAL_STRING("", m.end_date);
}

static void test_path_and_exists(void) {
    const class_rec_t* cls = setup_data();
    char path[80];
    export_path_for(cls, path, sizeof(path));
    TEST_ASSERT_EQUAL_STRING("/csv_export/CS101-M1.csv", path);
    TEST_ASSERT_FALSE(export_exists(cls));  // nothing written yet
}

static void test_write_csv_content(void) {
    const class_rec_t* cls = setup_data();
    export_result_t r = export_write_csv(cls);
    TEST_ASSERT_TRUE(r.ok);
    TEST_ASSERT_EQUAL_STRING("/csv_export/CS101-M1.csv", r.path);
    TEST_ASSERT_TRUE(r.size_bytes > 0);
    TEST_ASSERT_TRUE(export_exists(cls));

    // FREQ = absent days: 001 missed 2, 002 missed 1, 003 missed 0.
    char buf[256];
    read_file("/csv_export/CS101-M1.csv", buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("MATRICULA,FREQ\n202500001,2\n202500002,1\n202500003,0\n", buf);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)strlen(buf), r.size_bytes);
}

static void test_overwrite_replaces_not_appends(void) {
    const class_rec_t* cls = setup_data();
    export_write_csv(cls);
    // A later session where only 003 shows up bumps 001 and 002; re-export
    // must replace the file, not append.
    const char* c[] = {"202500003"};
    mark_day("2026-01-20", c, 1);
    export_result_t r = export_write_csv(cls);
    TEST_ASSERT_TRUE(r.ok);

    char buf[256];
    read_file("/csv_export/CS101-M1.csv", buf, sizeof(buf));
    // 4 days now: 001 missed 3, 002 missed 2, 003 missed 0. Single header only.
    TEST_ASSERT_EQUAL_STRING("MATRICULA,FREQ\n202500001,3\n202500002,2\n202500003,0\n", buf);
    TEST_ASSERT_NULL(strstr(buf + 1, "MATRICULA"));  // header not duplicated
}

static void test_export_preserves_open_session(void) {
    const class_rec_t* cls = setup_data();
    // A roll call is in progress on a fresh date.
    attendance_open(CLASS_DIR, "2026-02-01");
    attendance_set("202500002", true);

    export_write_csv(cls);  // opens historical dates internally

    // The in-progress session is intact afterward.
    TEST_ASSERT_TRUE(attendance_is_open());
    TEST_ASSERT_EQUAL_STRING("2026-02-01", attendance_date());
    TEST_ASSERT_EQUAL_STRING(CLASS_DIR, attendance_dir());
    TEST_ASSERT_TRUE(attendance_is_present("202500002"));
    TEST_ASSERT_EQUAL_INT(1, attendance_present_count());
}

int main(int, char**) {
    mock_freertos_set_delay_scale(1000);  // park roster_service's retry loop
    event_bus_init();
    UNITY_BEGIN();
    RUN_TEST(test_metrics);
    RUN_TEST(test_metrics_no_attendance);
    RUN_TEST(test_path_and_exists);
    RUN_TEST(test_write_csv_content);
    RUN_TEST(test_overwrite_replaces_not_appends);
    RUN_TEST(test_export_preserves_open_session);
    return UNITY_END();
}
