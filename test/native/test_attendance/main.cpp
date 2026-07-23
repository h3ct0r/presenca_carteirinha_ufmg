// attendance_store against the in-memory SD card: append-only JSONL sessions,
// last-write-wins fold, persistence across reopen, date listing.
//
// Ordering: the first test establishes the SD mount for the binary.

#include <stdio.h>
#include <string.h>
#include <unity.h>

#include "mock_sd.h"
#include "storage/attendance_store.h"

void setUp(void) {}
void tearDown(void) { attendance_close(); }

static const char* DIR = "CS101-M1";

static void read_file(const char* path, char* buf, size_t cap) {
    size_t n = mocksd_read_file(path, buf, cap - 1);
    buf[n] = '\0';
}

static void test_fresh_session_is_empty(void) {
    mocksd_reset();
    TEST_ASSERT_TRUE(attendance_open(DIR, "2026-07-16"));
    TEST_ASSERT_TRUE(attendance_is_open());
    TEST_ASSERT_EQUAL_STRING("2026-07-16", attendance_date());
    TEST_ASSERT_EQUAL_STRING(DIR, attendance_dir());
    TEST_ASSERT_EQUAL_INT(0, attendance_present_count());
}

static void test_mark_present_appends_and_updates(void) {
    mocksd_reset();
    attendance_open(DIR, "2026-07-16");
    TEST_ASSERT_TRUE(attendance_set("2023-0142", true));
    TEST_ASSERT_TRUE(attendance_set("2023-0187", true));
    TEST_ASSERT_TRUE(attendance_is_present("2023-0142"));
    TEST_ASSERT_TRUE(attendance_is_present("2023-0187"));
    TEST_ASSERT_FALSE(attendance_is_present("9999-9999"));
    TEST_ASSERT_EQUAL_INT(2, attendance_present_count());

    // Two JSONL lines were appended.
    char buf[512];
    read_file("/classes/CS101-M1/attendance/2026-07-16.jsonl", buf, sizeof(buf));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"2023-0142\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"present\":true"));
}

static void test_toggle_off_is_last_write_wins(void) {
    mocksd_reset();
    attendance_open(DIR, "2026-07-16");
    attendance_set("2023-0142", true);
    attendance_set("2023-0142", false);  // changed their mind
    TEST_ASSERT_FALSE(attendance_is_present("2023-0142"));
    TEST_ASSERT_EQUAL_INT(0, attendance_present_count());
    // Both lines are on disk (append-only, not rewritten).
    char buf[512];
    read_file("/classes/CS101-M1/attendance/2026-07-16.jsonl", buf, sizeof(buf));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"present\":true"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"present\":false"));
}

static void test_reopen_folds_persisted_state(void) {
    mocksd_reset();
    attendance_open(DIR, "2026-07-16");
    attendance_set("2023-0142", true);
    attendance_set("2023-0187", true);
    attendance_set("2023-0187", false);
    attendance_close();
    TEST_ASSERT_FALSE(attendance_is_open());

    // Reopening the same date reconstructs the present set from the log.
    TEST_ASSERT_TRUE(attendance_open(DIR, "2026-07-16"));
    TEST_ASSERT_TRUE(attendance_is_present("2023-0142"));
    TEST_ASSERT_FALSE(attendance_is_present("2023-0187"));
    TEST_ASSERT_EQUAL_INT(1, attendance_present_count());
}

static void test_sessions_are_separate_files(void) {
    mocksd_reset();
    attendance_open(DIR, "2026-07-14");
    attendance_set("2023-0142", true);
    attendance_open(DIR, "2026-07-16");  // different day, fresh
    TEST_ASSERT_EQUAL_INT(0, attendance_present_count());
    attendance_set("2023-0187", true);

    TEST_ASSERT_TRUE(mocksd_exists("/classes/CS101-M1/attendance/2026-07-14.jsonl"));
    TEST_ASSERT_TRUE(mocksd_exists("/classes/CS101-M1/attendance/2026-07-16.jsonl"));
}

static void test_list_dates_newest_first(void) {
    mocksd_reset();
    // Create three sessions out of order.
    attendance_open(DIR, "2026-07-14");
    attendance_set("a", true);
    attendance_open(DIR, "2026-07-20");
    attendance_set("a", true);
    attendance_open(DIR, "2026-07-09");
    attendance_set("a", true);
    attendance_close();

    char dates[8][12];
    int n = attendance_list_dates(DIR, dates, 8);
    TEST_ASSERT_EQUAL_INT(3, n);
    TEST_ASSERT_EQUAL_STRING("2026-07-20", dates[0]);
    TEST_ASSERT_EQUAL_STRING("2026-07-14", dates[1]);
    TEST_ASSERT_EQUAL_STRING("2026-07-09", dates[2]);

    TEST_ASSERT_EQUAL_INT(0, attendance_list_dates("NO-SUCH", dates, 8));
}

static void test_present_for_does_not_disturb_open_session(void) {
    mocksd_reset();
    attendance_open(DIR, "2026-07-14");
    attendance_set("a", true);
    attendance_set("b", true);
    attendance_close();

    attendance_open(DIR, "2026-07-16");  // current session
    attendance_set("c", true);

    // Querying a past date must not change the open session.
    TEST_ASSERT_EQUAL_INT(2, attendance_present_for(DIR, "2026-07-14"));
    TEST_ASSERT_EQUAL_INT(0, attendance_present_for(DIR, "2099-01-01"));  // missing file
    TEST_ASSERT_EQUAL_STRING("2026-07-16", attendance_date());
    TEST_ASSERT_EQUAL_INT(1, attendance_present_count());
    TEST_ASSERT_TRUE(attendance_is_present("c"));
}

static void test_clear_removes_all_sessions(void) {
    mocksd_reset();
    attendance_open(DIR, "2026-07-14");
    attendance_set("a", true);
    attendance_open(DIR, "2026-07-16");
    attendance_set("b", true);
    attendance_close();
    TEST_ASSERT_TRUE(mocksd_exists("/classes/CS101-M1/attendance/2026-07-14.jsonl"));

    int removed = attendance_clear(DIR);
    TEST_ASSERT_EQUAL_INT(2, removed);
    TEST_ASSERT_FALSE(mocksd_exists("/classes/CS101-M1/attendance/2026-07-14.jsonl"));
    TEST_ASSERT_FALSE(mocksd_exists("/classes/CS101-M1/attendance/2026-07-16.jsonl"));
    TEST_ASSERT_FALSE(attendance_is_open());

    char dates[8][12];
    TEST_ASSERT_EQUAL_INT(0, attendance_list_dates(DIR, dates, 8));
    TEST_ASSERT_EQUAL_INT(0, attendance_clear(DIR));  // idempotent: nothing left
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_fresh_session_is_empty);
    RUN_TEST(test_mark_present_appends_and_updates);
    RUN_TEST(test_toggle_off_is_last_write_wins);
    RUN_TEST(test_reopen_folds_persisted_state);
    RUN_TEST(test_sessions_are_separate_files);
    RUN_TEST(test_list_dates_newest_first);
    RUN_TEST(test_present_for_does_not_disturb_open_session);
    RUN_TEST(test_clear_removes_all_sessions);
    return UNITY_END();
}
