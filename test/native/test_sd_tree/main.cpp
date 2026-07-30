// sd_tree against the in-memory SD card: recursive delete and in-place rename
// for the debug web file manager.

#include <stdio.h>
#include <string.h>
#include <unity.h>

#include "mock_sd.h"
#include "storage/sd_tree.h"

void setUp(void) {}
void tearDown(void) {}

// A small tree: /classes/CS101-M1/{class.json,attendance/{2 sessions}} plus a
// loose file at the root.
static void seed(void) {
    mocksd_reset();
    mocksd_add_file("/classes/CS101-M1/class.json", "{}");
    mocksd_add_file("/classes/CS101-M1/attendance/2026-07-20.jsonl", "{}\n");
    mocksd_add_file("/classes/CS101-M1/attendance/2026-07-21.jsonl", "{}\n");
    mocksd_add_file("/config.json", "{}");
}

static void test_is_dir(void) {
    seed();
    TEST_ASSERT_TRUE(sd_tree_is_dir("/classes/CS101-M1"));
    TEST_ASSERT_TRUE(sd_tree_is_dir("/classes/CS101-M1/attendance"));
    TEST_ASSERT_FALSE(sd_tree_is_dir("/config.json"));
    TEST_ASSERT_FALSE(sd_tree_is_dir("/nope"));
}

static void test_remove_single_file(void) {
    seed();
    sd_tree_stats_t st = {0, 0};
    TEST_ASSERT_TRUE(sd_tree_remove("/config.json", &st, nullptr, 0));
    TEST_ASSERT_EQUAL_INT(1, st.removed);
    TEST_ASSERT_EQUAL_INT(0, st.failed);
    TEST_ASSERT_FALSE(mocksd_exists("/config.json"));
}

// The whole subtree goes: nested files, the nested directory, and the folder
// itself — while siblings are untouched.
static void test_remove_folder_is_recursive(void) {
    seed();
    sd_tree_stats_t st = {0, 0};
    TEST_ASSERT_TRUE(sd_tree_remove("/classes/CS101-M1", &st, nullptr, 0));
    TEST_ASSERT_EQUAL_INT(0, st.failed);
    TEST_ASSERT_EQUAL_INT(5, st.removed);  // 3 files + attendance/ + CS101-M1/
    TEST_ASSERT_FALSE(mocksd_exists("/classes/CS101-M1"));
    TEST_ASSERT_FALSE(mocksd_exists("/classes/CS101-M1/attendance"));
    TEST_ASSERT_FALSE(mocksd_exists("/classes/CS101-M1/attendance/2026-07-20.jsonl"));
    TEST_ASSERT_TRUE(mocksd_exists("/classes"));   // parent stays
    TEST_ASSERT_TRUE(mocksd_exists("/config.json"));
}

static void test_remove_trailing_slash_and_empty_dir(void) {
    seed();
    mocksd_add_dir("/photos");
    TEST_ASSERT_TRUE(sd_tree_remove("/photos/", nullptr, nullptr, 0));
    TEST_ASSERT_FALSE(mocksd_exists("/photos"));
}

static void test_remove_refuses_root_and_missing(void) {
    seed();
    char err[80] = "";
    TEST_ASSERT_FALSE(sd_tree_remove("/", nullptr, err, sizeof(err)));
    TEST_ASSERT_NOT_NULL(strstr(err, "root"));
    TEST_ASSERT_TRUE(mocksd_exists("/config.json"));  // nothing touched

    TEST_ASSERT_FALSE(sd_tree_remove("/does/not/exist", nullptr, err, sizeof(err)));
    TEST_ASSERT_EQUAL_STRING("not found", err);

    TEST_ASSERT_FALSE(sd_tree_remove("relative/path", nullptr, err, sizeof(err)));
    TEST_ASSERT_NOT_NULL(strstr(err, "absolute"));
}

static void test_rename_file_keeps_contents_and_folder(void) {
    seed();
    char err[80] = "";
    TEST_ASSERT_TRUE(sd_tree_rename("/classes/CS101-M1/class.json", "class.bak", err,
                                    sizeof(err)));
    TEST_ASSERT_FALSE(mocksd_exists("/classes/CS101-M1/class.json"));
    TEST_ASSERT_TRUE(mocksd_exists("/classes/CS101-M1/class.bak"));
    char buf[8];
    size_t n = mocksd_read_file("/classes/CS101-M1/class.bak", buf, sizeof(buf) - 1);
    buf[n] = '\0';
    TEST_ASSERT_EQUAL_STRING("{}", buf);
}

static void test_rename_file_in_root(void) {
    seed();
    TEST_ASSERT_TRUE(sd_tree_rename("/config.json", "config.old", nullptr, 0));
    TEST_ASSERT_FALSE(mocksd_exists("/config.json"));
    TEST_ASSERT_TRUE(mocksd_exists("/config.old"));
}

// Renaming a folder carries its whole subtree along.
static void test_rename_folder_moves_subtree(void) {
    seed();
    TEST_ASSERT_TRUE(sd_tree_rename("/classes/CS101-M1", "CS101-M2", nullptr, 0));
    TEST_ASSERT_FALSE(mocksd_exists("/classes/CS101-M1"));
    TEST_ASSERT_TRUE(mocksd_exists("/classes/CS101-M2"));
    TEST_ASSERT_TRUE(mocksd_exists("/classes/CS101-M2/class.json"));
    TEST_ASSERT_TRUE(mocksd_exists("/classes/CS101-M2/attendance/2026-07-20.jsonl"));
}

static void test_rename_rejects_bad_names(void) {
    seed();
    char err[80] = "";
    const char* bad[] = {"", ".", "..", "sub/dir", "back\\slash", "star*", "q?", "pipe|"};
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        err[0] = '\0';
        TEST_ASSERT_FALSE(sd_tree_rename("/config.json", bad[i], err, sizeof(err)));
        TEST_ASSERT_TRUE(err[0] != '\0');            // always explains why
        TEST_ASSERT_TRUE(mocksd_exists("/config.json"));  // and changes nothing
    }
}

static void test_rename_rejects_collision_and_missing(void) {
    seed();
    mocksd_add_file("/classes/CS101-M1/notes.txt", "hi");
    char err[80] = "";
    TEST_ASSERT_FALSE(sd_tree_rename("/classes/CS101-M1/notes.txt", "class.json", err,
                                     sizeof(err)));
    TEST_ASSERT_NOT_NULL(strstr(err, "already exists"));
    TEST_ASSERT_TRUE(mocksd_exists("/classes/CS101-M1/notes.txt"));  // still there

    TEST_ASSERT_FALSE(sd_tree_rename("/nope.txt", "yes.txt", err, sizeof(err)));
    TEST_ASSERT_EQUAL_STRING("not found", err);

    TEST_ASSERT_FALSE(sd_tree_rename("/", "card", err, sizeof(err)));
    TEST_ASSERT_NOT_NULL(strstr(err, "root"));
}

// Renaming to the name it already has is a no-op success, not a collision.
static void test_rename_to_same_name_is_noop(void) {
    seed();
    TEST_ASSERT_TRUE(sd_tree_rename("/config.json", "config.json", nullptr, 0));
    TEST_ASSERT_TRUE(mocksd_exists("/config.json"));
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_is_dir);
    RUN_TEST(test_remove_single_file);
    RUN_TEST(test_remove_folder_is_recursive);
    RUN_TEST(test_remove_trailing_slash_and_empty_dir);
    RUN_TEST(test_remove_refuses_root_and_missing);
    RUN_TEST(test_rename_file_keeps_contents_and_folder);
    RUN_TEST(test_rename_file_in_root);
    RUN_TEST(test_rename_folder_moves_subtree);
    RUN_TEST(test_rename_rejects_bad_names);
    RUN_TEST(test_rename_rejects_collision_and_missing);
    RUN_TEST(test_rename_to_same_name_is_noop);
    return UNITY_END();
}
