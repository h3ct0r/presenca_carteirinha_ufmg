// storage/checkin_store: the per-student check-in photo path + counter. Grouped
// under /students/checkins/<id>/<date>_<code>_<NN>.jpg.

#include <stdio.h>
#include <string.h>
#include <unity.h>

#include "mock_sd.h"
#include "storage/checkin_store.h"

void setUp(void) {}
void tearDown(void) {}

static void test_first_checkin_is_01(void) {
    mocksd_reset();
    char path[96];
    TEST_ASSERT_TRUE(checkin_store_next_path("2023-0142", "2026-07-28", "CS101-M1", path,
                                             sizeof(path)));
    TEST_ASSERT_EQUAL_STRING("/students/checkins/2023-0142/2026-07-28_CS101-M1_01.jpg", path);
}

static void test_counter_increments_past_existing(void) {
    mocksd_reset();
    mocksd_add_file("/students/checkins/2023-0142/2026-07-28_CS101-M1_01.jpg", "x");
    mocksd_add_file("/students/checkins/2023-0142/2026-07-28_CS101-M1_02.jpg", "x");
    char path[96];
    checkin_store_next_path("2023-0142", "2026-07-28", "CS101-M1", path, sizeof(path));
    TEST_ASSERT_EQUAL_STRING("/students/checkins/2023-0142/2026-07-28_CS101-M1_03.jpg", path);
}

static void test_prefix_scoped_by_date_and_class(void) {
    mocksd_reset();
    // Files from a different day / class must not bump this prefix's counter.
    mocksd_add_file("/students/checkins/2023-0142/2026-07-27_CS101-M1_09.jpg", "x");
    mocksd_add_file("/students/checkins/2023-0142/2026-07-28_MA110-F1_04.jpg", "x");
    char path[96];
    checkin_store_next_path("2023-0142", "2026-07-28", "CS101-M1", path, sizeof(path));
    TEST_ASSERT_EQUAL_STRING("/students/checkins/2023-0142/2026-07-28_CS101-M1_01.jpg", path);
}

static void test_per_student_folders(void) {
    mocksd_reset();
    mocksd_add_file("/students/checkins/2023-0142/2026-07-28_CS101-M1_01.jpg", "x");
    char path[96];
    // A different student starts fresh at 01.
    checkin_store_next_path("2023-0187", "2026-07-28", "CS101-M1", path, sizeof(path));
    TEST_ASSERT_EQUAL_STRING("/students/checkins/2023-0187/2026-07-28_CS101-M1_01.jpg", path);
}

static void test_rejects_empty_id(void) {
    char path[96];
    TEST_ASSERT_FALSE(checkin_store_next_path("", "2026-07-28", "CS101-M1", path, sizeof(path)));
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_first_checkin_is_01);
    RUN_TEST(test_counter_increments_past_existing);
    RUN_TEST(test_prefix_scoped_by_date_and_class);
    RUN_TEST(test_per_student_folders);
    RUN_TEST(test_rejects_empty_id);
    return UNITY_END();
}
