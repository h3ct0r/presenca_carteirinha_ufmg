// atomic_file: replacing a file so that a power cut at any instant still leaves
// a complete copy on the card. The point of the module is what survives an
// interruption, so most of these tests simulate one by leaving the card in the
// exact state a cut would produce and then running the recovery.

#include <string.h>
#include <unity.h>

#include "SD_MMC.h"
#include "mock_sd.h"
#include "storage/atomic_file.h"

void setUp(void) { mocksd_reset(); }
void tearDown(void) {}

static const char* PATH = "/config.json";
static const char* BAK = "/config.json.bak";
static const char* TMP = "/config.json.tmp";

static bool write_str(const char* path, const char* s) {
    return atomic_file_write(path, (const uint8_t*)s, strlen(s), "test");
}

static void assert_contents(const char* path, const char* expected) {
    char buf[256] = {0};
    size_t n = mocksd_read_file(path, buf, sizeof(buf) - 1);
    buf[n] = '\0';
    TEST_ASSERT_EQUAL_STRING(expected, buf);
}

// --- the happy path ---------------------------------------------------------

static void test_write_creates_a_new_file(void) {
    TEST_ASSERT_TRUE(write_str(PATH, "first"));
    assert_contents(PATH, "first");
    // No litter: a reader that saw either of these would be misled.
    TEST_ASSERT_FALSE(mocksd_exists(BAK));
    TEST_ASSERT_FALSE(mocksd_exists(TMP));
}

static void test_write_replaces_an_existing_file(void) {
    mocksd_add_file(PATH, "old");
    TEST_ASSERT_TRUE(write_str(PATH, "new"));
    assert_contents(PATH, "new");
    TEST_ASSERT_FALSE(mocksd_exists(BAK));
    TEST_ASSERT_FALSE(mocksd_exists(TMP));
}

static void test_repeated_writes_leave_no_litter(void) {
    for (int i = 0; i < 5; i++) TEST_ASSERT_TRUE(write_str(PATH, "x"));
    assert_contents(PATH, "x");
    TEST_ASSERT_FALSE(mocksd_exists(BAK));
    TEST_ASSERT_FALSE(mocksd_exists(TMP));
}

// --- what a power cut leaves, and what recovery does with it ----------------

// Cut between steps 2 and 3: the original was moved aside and the replacement
// never landed. This is the window the old remove-then-rename could not survive
// — without recovery the device would boot with NO config.json at all and offer
// to set itself up from scratch over the top of a real configuration.
static void test_recover_restores_when_only_the_backup_exists(void) {
    mocksd_add_file(BAK, "the original");
    TEST_ASSERT_FALSE(mocksd_exists(PATH));

    TEST_ASSERT_TRUE(atomic_file_recover(PATH));
    assert_contents(PATH, "the original");
    TEST_ASSERT_FALSE(mocksd_exists(BAK));
}

// Cut between steps 3 and 4: the new file is already in place and the backup is
// just leftovers. Recovery must NOT overwrite the newer file with the older one.
static void test_recover_drops_a_stale_backup_without_clobbering(void) {
    mocksd_add_file(PATH, "the new one");
    mocksd_add_file(BAK, "the old one");

    TEST_ASSERT_FALSE(atomic_file_recover(PATH));  // nothing to recover
    assert_contents(PATH, "the new one");
    TEST_ASSERT_FALSE(mocksd_exists(BAK));
}

static void test_recover_is_a_no_op_on_a_clean_card(void) {
    mocksd_add_file(PATH, "fine");
    TEST_ASSERT_FALSE(atomic_file_recover(PATH));
    assert_contents(PATH, "fine");
}

// A genuinely absent file must stay absent — recovery must not invent one.
static void test_recover_does_not_create_a_missing_file(void) {
    TEST_ASSERT_FALSE(atomic_file_recover(PATH));
    TEST_ASSERT_FALSE(mocksd_exists(PATH));
}

static void test_recover_is_idempotent(void) {
    mocksd_add_file(BAK, "original");
    TEST_ASSERT_TRUE(atomic_file_recover(PATH));
    TEST_ASSERT_FALSE(atomic_file_recover(PATH));
    TEST_ASSERT_FALSE(atomic_file_recover(PATH));
    assert_contents(PATH, "original");
}

// A write that starts while a stale .bak is lying around must still work, and
// must not leave the stale one behind to be "recovered" later.
static void test_write_clears_a_stale_backup(void) {
    mocksd_add_file(PATH, "current");
    mocksd_add_file(BAK, "ancient");
    TEST_ASSERT_TRUE(write_str(PATH, "newest"));
    assert_contents(PATH, "newest");
    TEST_ASSERT_FALSE(mocksd_exists(BAK));
}

// --- failures must leave the original reachable ------------------------------

// A full card is the common failure. The original must survive it untouched:
// losing the roster because the card filled up would be the worst of both.
static void test_full_card_leaves_the_original_intact(void) {
    mocksd_add_file(PATH, "precious");
    mocksd_set_card_full(true);

    TEST_ASSERT_FALSE(write_str(PATH, "replacement"));

    mocksd_set_card_full(false);
    assert_contents(PATH, "precious");
    TEST_ASSERT_FALSE(mocksd_exists(TMP));  // no truncated leftover
    // And nothing for recovery to misinterpret on the next boot.
    TEST_ASSERT_FALSE(atomic_file_recover(PATH));
    assert_contents(PATH, "precious");
}

static void test_path_too_long_is_refused(void) {
    char longpath[200];
    memset(longpath, 'a', sizeof(longpath) - 1);
    longpath[0] = '/';
    longpath[sizeof(longpath) - 1] = '\0';
    TEST_ASSERT_FALSE(write_str(longpath, "nope"));
}

static void test_empty_content_is_written(void) {
    mocksd_add_file(PATH, "was here");
    TEST_ASSERT_TRUE(atomic_file_write(PATH, (const uint8_t*)"", 0, "test"));
    assert_contents(PATH, "");
    TEST_ASSERT_FALSE(mocksd_exists(BAK));
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_write_creates_a_new_file);
    RUN_TEST(test_write_replaces_an_existing_file);
    RUN_TEST(test_repeated_writes_leave_no_litter);
    RUN_TEST(test_recover_restores_when_only_the_backup_exists);
    RUN_TEST(test_recover_drops_a_stale_backup_without_clobbering);
    RUN_TEST(test_recover_is_a_no_op_on_a_clean_card);
    RUN_TEST(test_recover_does_not_create_a_missing_file);
    RUN_TEST(test_recover_is_idempotent);
    RUN_TEST(test_write_clears_a_stale_backup);
    RUN_TEST(test_full_card_leaves_the_original_intact);
    RUN_TEST(test_path_too_long_is_refused);
    RUN_TEST(test_empty_content_is_written);
    return UNITY_END();
}
