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

    int failed = -1;
    int removed = attendance_clear(DIR, &failed);
    TEST_ASSERT_EQUAL_INT(2, removed);
    TEST_ASSERT_EQUAL_INT(0, failed);  // nothing left behind
    TEST_ASSERT_FALSE(mocksd_exists("/classes/CS101-M1/attendance/2026-07-14.jsonl"));
    TEST_ASSERT_FALSE(mocksd_exists("/classes/CS101-M1/attendance/2026-07-16.jsonl"));
    TEST_ASSERT_FALSE(attendance_is_open());

    char dates[8][12];
    TEST_ASSERT_EQUAL_INT(0, attendance_list_dates(DIR, dates, 8));
    TEST_ASSERT_EQUAL_INT(0, attendance_clear(DIR, nullptr));  // idempotent; NULL out-param ok
}

// --- timed (double-tap) attendance ------------------------------------------

static const long long MIN_US = 60000000LL;  // one minute in microseconds

static void test_timed_present_when_over_threshold(void) {
    mocksd_reset();
    attendance_open(DIR, "2026-07-20");

    // Tap in at t=0: in progress, not yet counted, whole threshold still to wait.
    att_state_t s = attendance_tap("2023-0142", 0, 45);
    TEST_ASSERT_EQUAL_INT(ATT_IN_PROGRESS, s.status);
    TEST_ASSERT_EQUAL_INT(45, s.remaining);
    TEST_ASSERT_FALSE(attendance_is_present("2023-0142"));
    TEST_ASSERT_EQUAL_INT(0, attendance_present_count());

    // A poll partway through reports the running minutes and the countdown.
    s = attendance_tap_state("2023-0142", 30 * MIN_US, 45);
    TEST_ASSERT_EQUAL_INT(ATT_IN_PROGRESS, s.status);
    TEST_ASSERT_EQUAL_INT(30, s.minutes);
    TEST_ASSERT_EQUAL_INT(15, s.remaining);

    // Confirming tap at t=52 min: present, minutes recorded and persisted.
    s = attendance_tap("2023-0142", 52 * MIN_US, 45);
    TEST_ASSERT_EQUAL_INT(ATT_PRESENT, s.status);
    TEST_ASSERT_EQUAL_INT(52, s.minutes);
    TEST_ASSERT_EQUAL_INT(0, s.remaining);
    TEST_ASSERT_TRUE(attendance_is_present("2023-0142"));
    TEST_ASSERT_EQUAL_INT(1, attendance_present_count());

    char buf[256];
    read_file("/classes/CS101-M1/attendance/2026-07-20.jsonl", buf, sizeof(buf));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"min\":52"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"present\":true"));
}

// A tap before the threshold changes nothing: no record, and the arrival stands
// so the student can come back later and still be registered.
static void test_timed_early_tap_is_not_recorded(void) {
    mocksd_reset();
    attendance_open(DIR, "2026-07-20");
    attendance_tap("2023-0142", 0, 45);
    att_state_t s = attendance_tap("2023-0142", 12 * MIN_US, 45);  // 33 min too soon
    TEST_ASSERT_EQUAL_INT(ATT_TOO_EARLY, s.status);
    TEST_ASSERT_EQUAL_INT(12, s.minutes);
    TEST_ASSERT_EQUAL_INT(33, s.remaining);
    TEST_ASSERT_FALSE(attendance_is_present("2023-0142"));
    TEST_ASSERT_EQUAL_INT(0, attendance_present_count());
    // Nothing was written for the rejected tap.
    TEST_ASSERT_FALSE(mocksd_exists("/classes/CS101-M1/attendance/2026-07-20.jsonl"));

    // The original arrival is still the reference: waiting it out registers.
    s = attendance_tap("2023-0142", 50 * MIN_US, 45);
    TEST_ASSERT_EQUAL_INT(ATT_PRESENT, s.status);
    TEST_ASSERT_EQUAL_INT(50, s.minutes);
    TEST_ASSERT_TRUE(attendance_is_present("2023-0142"));
}

// The countdown rounds up, so a student mid-minute is never told "0 min left"
// while the threshold is not actually met.
static void test_timed_remaining_rounds_up(void) {
    mocksd_reset();
    attendance_open(DIR, "2026-07-20");
    attendance_tap("2023-0142", 0, 45);
    att_state_t s = attendance_tap("2023-0142", 44 * MIN_US + MIN_US / 2, 45);
    TEST_ASSERT_EQUAL_INT(ATT_TOO_EARLY, s.status);
    TEST_ASSERT_EQUAL_INT(44, s.minutes);
    TEST_ASSERT_EQUAL_INT(1, s.remaining);
}

// Once registered, further taps are ignored — reported, but never re-recorded.
static void test_timed_extra_tap_after_present_is_ignored(void) {
    mocksd_reset();
    attendance_open(DIR, "2026-07-20");
    attendance_tap("2023-0142", 0, 45);
    attendance_tap("2023-0142", 50 * MIN_US, 45);  // registered, 50 min

    char before[256];
    read_file("/classes/CS101-M1/attendance/2026-07-20.jsonl", before, sizeof(before));

    att_state_t s = attendance_tap("2023-0142", 80 * MIN_US, 45);
    TEST_ASSERT_EQUAL_INT(ATT_ALREADY_PRESENT, s.status);
    TEST_ASSERT_EQUAL_INT(50, s.minutes);  // still the registered duration
    TEST_ASSERT_EQUAL_INT(1, attendance_present_count());

    char after[256];
    read_file("/classes/CS101-M1/attendance/2026-07-20.jsonl", after, sizeof(after));
    TEST_ASSERT_EQUAL_STRING(before, after);  // no extra line
}

static void test_timed_no_confirm_is_absent(void) {
    mocksd_reset();
    attendance_open(DIR, "2026-07-20");
    attendance_tap("2023-0142", 0, 45);  // in, never confirmed
    // Still in progress live...
    TEST_ASSERT_EQUAL_INT(ATT_IN_PROGRESS,
                          attendance_tap_state("2023-0142", 90 * MIN_US, 45).status);
    // ...but not present, and a reopen (in-progress is RAM-only) shows absent.
    TEST_ASSERT_FALSE(attendance_is_present("2023-0142"));
    attendance_open(DIR, "2026-07-20");
    TEST_ASSERT_EQUAL_INT(0, attendance_present_count());
    TEST_ASSERT_EQUAL_INT(ATT_ABSENT, attendance_tap_state("2023-0142", 0, 45).status);
}

static void test_manual_override_clears_in_progress(void) {
    mocksd_reset();
    attendance_open(DIR, "2026-07-20");
    attendance_tap("2023-0142", 0, 45);  // in progress
    TEST_ASSERT_TRUE(attendance_set("2023-0142", true));  // professor forces present
    TEST_ASSERT_TRUE(attendance_is_present("2023-0142"));
    // The in-progress record was cleared and the student counts as present, so a
    // later tap is just ignored.
    att_state_t s = attendance_tap("2023-0142", 10 * MIN_US, 45);
    TEST_ASSERT_EQUAL_INT(ATT_ALREADY_PRESENT, s.status);
}

static void test_timed_present_survives_reopen(void) {
    mocksd_reset();
    attendance_open(DIR, "2026-07-20");
    attendance_tap("2023-0142", 0, 45);
    attendance_tap("2023-0142", 50 * MIN_US, 45);  // present, 50 min
    attendance_open(DIR, "2026-07-20");            // reopen folds the file
    TEST_ASSERT_TRUE(attendance_is_present("2023-0142"));
    att_state_t s = attendance_tap_state("2023-0142", 0, 45);
    TEST_ASSERT_EQUAL_INT(ATT_PRESENT, s.status);
    TEST_ASSERT_EQUAL_INT(50, s.minutes);
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
    RUN_TEST(test_timed_present_when_over_threshold);
    RUN_TEST(test_timed_left_early_not_present);
    RUN_TEST(test_timed_no_tapout_is_absent);
    RUN_TEST(test_manual_override_clears_in_progress);
    RUN_TEST(test_timed_present_survives_reopen);
    return UNITY_END();
}
