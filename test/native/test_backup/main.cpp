// storage/backup_store against the in-memory SD card: snapshots the authored
// config surface (/config.json, /students/students.json, /classes/*/class.json)
// into /backup/previous, skipping missing files and never touching
// device-produced data (attendance/photos).

#include <stdio.h>
#include <string.h>
#include <unity.h>

#include "mock_sd.h"
#include "storage/backup_store.h"

void setUp(void) {}
void tearDown(void) {}

// Reads a mock file into a NUL-terminated buffer for comparison.
static void read_file(const char* path, char* out, size_t cap) {
    size_t n = mocksd_read_file(path, out, cap - 1);
    out[n] = '\0';
}

static void test_snapshots_all_authored_files(void) {
    mocksd_reset();
    mocksd_add_file("/config.json", "{\"cfg\":1}");
    mocksd_add_file("/students/students.json", "{\"students\":[]}");
    mocksd_add_file("/classes/CS101-M1/class.json", "{\"code\":\"CS101-M1\"}");
    mocksd_add_file("/classes/MA110-F1/class.json", "{\"code\":\"MA110-F1\"}");

    backup_result_t r = backup_store_create();
    TEST_ASSERT_TRUE(r.ok);
    TEST_ASSERT_NOT_NULL(strstr(r.message, "4 file"));

    char buf[128];
    read_file("/backup/previous/config.json", buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("{\"cfg\":1}", buf);
    read_file("/backup/previous/students/students.json", buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("{\"students\":[]}", buf);
    read_file("/backup/previous/classes/CS101-M1/class.json", buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("{\"code\":\"CS101-M1\"}", buf);
    read_file("/backup/previous/classes/MA110-F1/class.json", buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("{\"code\":\"MA110-F1\"}", buf);

    TEST_ASSERT_TRUE(backup_store_exists());
    TEST_ASSERT_EQUAL_STRING("/backup/previous", backup_store_root());
}

static void test_skips_missing_files_on_fresh_device(void) {
    mocksd_reset();
    mocksd_add_file("/config.json", "{\"only\":\"config\"}");  // no students, no classes

    backup_result_t r = backup_store_create();
    TEST_ASSERT_TRUE(r.ok);
    TEST_ASSERT_NOT_NULL(strstr(r.message, "1 file"));
    TEST_ASSERT_FALSE(mocksd_exists("/backup/previous/students/students.json"));
    TEST_ASSERT_TRUE(backup_store_exists());
}

static void test_no_config_means_no_snapshot(void) {
    mocksd_reset();  // truly blank card
    backup_result_t r = backup_store_create();
    TEST_ASSERT_TRUE(r.ok);  // nothing to do is still success
    TEST_ASSERT_NOT_NULL(strstr(r.message, "0 file"));
    TEST_ASSERT_FALSE(backup_store_exists());
}

static void test_preserves_attendance_and_photos(void) {
    mocksd_reset();
    mocksd_add_file("/config.json", "{\"cfg\":1}");
    mocksd_add_file("/classes/CS101-M1/class.json", "{\"code\":\"CS101-M1\"}");
    mocksd_add_file("/classes/CS101-M1/attendance/2026-07-27.jsonl", "{\"id\":\"x\"}");
    mocksd_add_file("/photos/2023-0142.jpg", "JPEGDATA");

    backup_result_t r = backup_store_create();
    TEST_ASSERT_TRUE(r.ok);

    // Originals untouched.
    TEST_ASSERT_TRUE(mocksd_exists("/classes/CS101-M1/attendance/2026-07-27.jsonl"));
    TEST_ASSERT_TRUE(mocksd_exists("/photos/2023-0142.jpg"));
    // And never copied into the snapshot.
    TEST_ASSERT_FALSE(mocksd_exists("/backup/previous/classes/CS101-M1/attendance/2026-07-27.jsonl"));
    TEST_ASSERT_FALSE(mocksd_exists("/backup/previous/photos/2023-0142.jpg"));
}

static void test_overwrites_prior_snapshot(void) {
    mocksd_reset();
    mocksd_add_file("/config.json", "{\"v\":1}");
    TEST_ASSERT_TRUE(backup_store_create().ok);
    mocksd_add_file("/config.json", "{\"v\":2}");  // config changed
    TEST_ASSERT_TRUE(backup_store_create().ok);

    char buf[64];
    read_file("/backup/previous/config.json", buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("{\"v\":2}", buf);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_snapshots_all_authored_files);
    RUN_TEST(test_skips_missing_files_on_fresh_device);
    RUN_TEST(test_no_config_means_no_snapshot);
    RUN_TEST(test_preserves_attendance_and_photos);
    RUN_TEST(test_overwrites_prior_snapshot);
    return UNITY_END();
}
