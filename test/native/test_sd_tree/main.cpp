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

// --- wipe the card root, sparing one file -----------------------------------

// A card as the device leaves it: authored config + everything it produced.
static void seed_full_card(void) {
    seed();
    mocksd_add_file("/students/students.json", "{}");
    mocksd_add_file("/students/photos/2023-0142.jpg", "jpegbytes");
    mocksd_add_file("/students/checkins/2023-0142/2026-07-20_CS101_01.jpg", "jpegbytes");
    mocksd_add_file("/photos/IMG_0001.jpg", "jpegbytes");
    mocksd_add_file("/exports/CS101-M1_2026-07-20.csv", "a;b\n");
    mocksd_add_file("/models/face_detect.espdl", "model");
    mocksd_add_file("/backup/previous/config.json", "{}");
}

static void test_wipe_keeps_only_config(void) {
    seed_full_card();
    sd_tree_stats_t st = {0, 0};
    char err[80] = "";
    TEST_ASSERT_TRUE(sd_tree_wipe_root("config.json", &st, err, sizeof(err)));
    TEST_ASSERT_EQUAL_INT(0, st.failed);
    TEST_ASSERT_TRUE(st.removed > 0);

    TEST_ASSERT_TRUE(mocksd_exists("/config.json"));  // the one survivor
    const char* gone[] = {
        "/classes", "/classes/CS101-M1", "/classes/CS101-M1/class.json",
        "/classes/CS101-M1/attendance/2026-07-20.jsonl",
        "/students", "/students/students.json", "/students/photos/2023-0142.jpg",
        "/students/checkins/2023-0142/2026-07-20_CS101_01.jpg",
        "/photos", "/photos/IMG_0001.jpg", "/exports", "/models", "/backup",
    };
    for (unsigned i = 0; i < sizeof(gone) / sizeof(gone[0]); i++) {
        TEST_ASSERT_FALSE_MESSAGE(mocksd_exists(gone[i]), gone[i]);
    }
}

// Config lives at the root, so a same-named file deeper in the tree must NOT be
// spared — only the root entry is.
static void test_wipe_spares_only_the_root_copy(void) {
    seed_full_card();
    TEST_ASSERT_TRUE(sd_tree_wipe_root("config.json", nullptr, nullptr, 0));
    TEST_ASSERT_TRUE(mocksd_exists("/config.json"));
    TEST_ASSERT_FALSE(mocksd_exists("/backup/previous/config.json"));
}

// FAT is case-insensitive, so CONFIG.JSON and config.json are the same file.
static void test_wipe_keep_match_is_case_insensitive(void) {
    mocksd_reset();
    mocksd_add_file("/CONFIG.JSON", "{}");
    mocksd_add_file("/classes/X/class.json", "{}");
    TEST_ASSERT_TRUE(sd_tree_wipe_root("config.json", nullptr, nullptr, 0));
    TEST_ASSERT_TRUE(mocksd_exists("/CONFIG.JSON"));
    TEST_ASSERT_FALSE(mocksd_exists("/classes"));
}

static void test_wipe_on_a_card_without_config_is_still_clean(void) {
    mocksd_reset();
    mocksd_add_file("/classes/X/class.json", "{}");
    mocksd_add_file("/stray.txt", "x");
    sd_tree_stats_t st = {0, 0};
    TEST_ASSERT_TRUE(sd_tree_wipe_root("config.json", &st, nullptr, 0));
    TEST_ASSERT_EQUAL_INT(0, st.failed);
    TEST_ASSERT_FALSE(mocksd_exists("/classes"));
    TEST_ASSERT_FALSE(mocksd_exists("/stray.txt"));
}

static void test_wipe_of_an_empty_card_is_a_no_op(void) {
    mocksd_reset();
    mocksd_add_file("/config.json", "{}");
    sd_tree_stats_t st = {0, 0};
    TEST_ASSERT_TRUE(sd_tree_wipe_root("config.json", &st, nullptr, 0));
    TEST_ASSERT_EQUAL_INT(0, st.removed);
    TEST_ASSERT_EQUAL_INT(0, st.failed);
    TEST_ASSERT_TRUE(mocksd_exists("/config.json"));
}

// Running it twice must be safe and leave the same card.
static void test_wipe_is_idempotent(void) {
    seed_full_card();
    TEST_ASSERT_TRUE(sd_tree_wipe_root("config.json", nullptr, nullptr, 0));
    sd_tree_stats_t st = {0, 0};
    TEST_ASSERT_TRUE(sd_tree_wipe_root("config.json", &st, nullptr, 0));
    TEST_ASSERT_EQUAL_INT(0, st.removed);
    TEST_ASSERT_TRUE(mocksd_exists("/config.json"));
}

// An empty/NULL keep means "spare nothing" — config.json goes too.
static void test_wipe_with_no_keep_removes_everything(void) {
    seed_full_card();
    TEST_ASSERT_TRUE(sd_tree_wipe_root(nullptr, nullptr, nullptr, 0));
    TEST_ASSERT_FALSE(mocksd_exists("/config.json"));
    TEST_ASSERT_FALSE(mocksd_exists("/classes"));
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
    RUN_TEST(test_wipe_keeps_only_config);
    RUN_TEST(test_wipe_spares_only_the_root_copy);
    RUN_TEST(test_wipe_keep_match_is_case_insensitive);
    RUN_TEST(test_wipe_on_a_card_without_config_is_still_clean);
    RUN_TEST(test_wipe_of_an_empty_card_is_a_no_op);
    RUN_TEST(test_wipe_is_idempotent);
    RUN_TEST(test_wipe_with_no_keep_removes_everything);
    return UNITY_END();
}
