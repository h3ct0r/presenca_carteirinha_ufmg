// services/import_service against the in-memory SD card: the full offline
// config-import pipeline (backup -> unpack -> validate -> apply -> reload),
// plus the safety guarantees — a bad archive or an invalid staged config must
// leave the live config byte-intact, and revert restores the snapshot.

#include <stdio.h>
#include <string.h>
#include <unity.h>

#include "FS.h"
#include "SD_MMC.h"
#include "app/event_bus.h"
#include "mock_freertos.h"
#include "mock_sd.h"
#include "services/config_service.h"
#include "services/import_service.h"
#include "services/roster_service.h"

void setUp(void) {
    app_event_t ev;
    while (event_bus_poll(&ev)) {
    }
}
void tearDown(void) {}

// --- ustar archive builder (mirrors tools/config-builder/src/tarball.js) -----

static void put(uint8_t* h, const char* s, int off, int len) {
    for (int i = 0; i < len && s[i]; i++) h[off + i] = (uint8_t)s[i];
}
static void build_header(uint8_t h[512], const char* name, size_t size, char typeflag) {
    memset(h, 0, 512);
    put(h, name, 0, 100);
    put(h, "0000644", 100, 7);
    put(h, "0000000", 108, 7);
    put(h, "0000000", 116, 7);
    char so[16];
    snprintf(so, sizeof(so), "%011lo", (unsigned long)size);
    put(h, so, 124, 11);
    put(h, "00000000000", 136, 11);
    for (int i = 148; i < 156; i++) h[i] = ' ';
    h[156] = (uint8_t)typeflag;
    put(h, "ustar", 257, 5);
    h[263] = '0';
    h[264] = '0';
    unsigned sum = 0;
    for (int i = 0; i < 512; i++) sum += h[i];
    char cs[8];
    snprintf(cs, sizeof(cs), "%06o", sum);
    put(h, cs, 148, 6);
    h[154] = '\0';
    h[155] = ' ';
}
struct TarBuf {
    uint8_t b[8192];
    size_t len = 0;
    void add(const char* name, const char* data) {
        size_t size = data ? strlen(data) : 0;
        build_header(b + len, name, size, '0');
        len += 512;
        if (size) {
            memcpy(b + len, data, size);
            len += ((size + 511) / 512) * 512;
        }
    }
    void end() {
        memset(b + len, 0, 1024);
        len += 1024;
    }
    void stage_at(const char* path) {  // write the archive bytes as a binary SD file
        File f = SD_MMC.open(path, "w", true);
        f.write(b, len);
        f.close();
    }
};

// --- fixtures ---------------------------------------------------------------

static const char* LIVE_CONFIG =
    "{ \"teachers\": [ { \"name\": \"Old Prof\", \"email\": \"old@x\", "
    "\"password\": \"1111\" } ] }";
static const char* LIVE_STUDENTS =
    "{ \"version\": 1, \"students\": [ { \"id\": \"A\", \"name\": \"Alice\" }, "
    "{ \"id\": \"B\", \"name\": \"Bob\" } ] }";
static const char* LIVE_CLASS =
    "{ \"version\": 1, \"code\": \"CS101-M1\", \"name\": \"DS\", "
    "\"roster\": [ { \"id\": \"A\" }, { \"id\": \"B\" } ] }";

static const char* NEW_CONFIG =
    "{ \"teachers\": [ { \"name\": \"New A\", \"email\": \"a@x\", \"password\": \"2222\" }, "
    "{ \"name\": \"New B\", \"email\": \"b@x\", \"password\": \"3333\" } ] }";
static const char* NEW_STUDENTS =
    "{ \"version\": 1, \"students\": [ { \"id\": \"A\", \"name\": \"Alice New\" }, "
    "{ \"id\": \"B\", \"name\": \"Bob New\" } ] }";
static const char* NEW_CLASS =
    "{ \"version\": 1, \"code\": \"CS101-M1\", \"name\": \"DS New\", "
    "\"roster\": [ { \"id\": \"A\" }, { \"id\": \"B\" } ] }";

static void setup_live(void) {
    mocksd_reset();
    mocksd_add_file("/config.json", LIVE_CONFIG);
    mocksd_add_file("/students/students.json", LIVE_STUDENTS);
    mocksd_add_file("/classes/CS101-M1/class.json", LIVE_CLASS);
    config_service_start();
    roster_service_start();
    TEST_ASSERT_EQUAL(CONFIG_OK, config_get_status());
    TEST_ASSERT_EQUAL(ROSTER_OK, roster_get_status());
}

static int teacher_count(void) {
    device_config_t c;
    config_get(&c);
    return c.teacher_count;
}

// --- tests ------------------------------------------------------------------

static void test_import_applies_and_backs_up(void) {
    setup_live();
    TEST_ASSERT_EQUAL(1, teacher_count());

    TarBuf t;
    t.add("config.json", NEW_CONFIG);
    t.add("students/students.json", NEW_STUDENTS);
    t.add("classes/CS101-M1/class.json", NEW_CLASS);
    t.end();
    t.stage_at("/config.tar");
    TEST_ASSERT_TRUE(import_service_pending());

    import_result_t r = import_service_run("/config.tar");
    TEST_ASSERT_TRUE_MESSAGE(r.ok, r.message);

    // Live config + roster now reflect the imported data.
    TEST_ASSERT_EQUAL(2, teacher_count());
    TEST_ASSERT_EQUAL(ROSTER_OK, roster_get_status());
    TEST_ASSERT_EQUAL(2, roster_student_count());

    // The previous config was snapshotted.
    char buf[128];
    size_t n = mocksd_read_file("/backup/previous/config.json", buf, sizeof(buf) - 1);
    buf[n] = '\0';
    TEST_ASSERT_EQUAL_STRING(LIVE_CONFIG, buf);

    // The source was sentinelled so it won't re-import on the next boot.
    TEST_ASSERT_FALSE(import_service_pending());
    TEST_ASSERT_TRUE(mocksd_exists("/config.tar.imported"));
}

// Reported bug: after importing a config whose classes have DIFFERENT codes, the
// class list showed "No class data" and the old class folder was still on the
// card. An import is an overlay (it never deletes another professor's class), so
// the stale /classes/<OLD>/class.json survives — and its roster references
// students that the new students.json no longer has. The loader used to treat
// that as fatal and dropped the ENTIRE roster, hiding the freshly imported
// classes too. A stale class must be skipped, not take everything down with it.
static void test_stale_class_from_previous_config_does_not_hide_new_classes(void) {
    setup_live();  // students A,B + class CS101-M1 whose roster is [A,B]
    TEST_ASSERT_EQUAL(ROSTER_OK, roster_get_status());
    TEST_ASSERT_EQUAL_INT(1, roster_class_count());

    // A completely different config: new student ids and a new class code.
    static const char* OTHER_STUDENTS =
        "{ \"version\": 1, \"students\": [ { \"id\": \"X\", \"name\": \"Xavier\" }, "
        "{ \"id\": \"Y\", \"name\": \"Yara\" } ] }";
    static const char* OTHER_CLASS =
        "{ \"version\": 1, \"code\": \"DCC219\", \"name\": \"Robotics\", "
        "\"roster\": [ { \"id\": \"X\" }, { \"id\": \"Y\" } ] }";

    TarBuf t;
    t.add("config.json", NEW_CONFIG);
    t.add("students/students.json", OTHER_STUDENTS);
    t.add("classes/DCC219/class.json", OTHER_CLASS);
    t.end();
    t.stage_at("/config.tar");

    import_result_t r = import_service_run("/config.tar");
    TEST_ASSERT_TRUE_MESSAGE(r.ok, r.message);

    // The old class folder is still there (overlay semantics, attendance safe)...
    TEST_ASSERT_TRUE(mocksd_exists("/classes/CS101-M1/class.json"));
    // ...but the roster still loads, and the NEW class is visible.
    TEST_ASSERT_EQUAL_MESSAGE(ROSTER_OK, roster_get_status(),
                              "a stale class must not blank the whole class list");
    TEST_ASSERT_EQUAL_INT(1, roster_class_count());
    TEST_ASSERT_EQUAL_STRING("DCC219", roster_class_at(0)->code);
}

static void test_import_applies_avatars_and_preserves_device_photos(void) {
    setup_live();
    // Device-produced trees that MUST survive an import (never named by apply).
    mocksd_add_file("/students/checkins/A/2026-07-28_CS101_01.jpg", "CHECKIN-A");
    mocksd_add_file("/photos/snap.jpg", "DEVICE-SNAP");

    TarBuf t;
    t.add("config.json", NEW_CONFIG);
    t.add("students/students.json", NEW_STUDENTS);
    t.add("classes/CS101-M1/class.json", NEW_CLASS);
    t.add("students/photos/A.jpg", "AVATAR-A-BYTES");  // authored avatar
    t.end();
    t.stage_at("/config.tar");

    import_result_t r = import_service_run("/config.tar");
    TEST_ASSERT_TRUE_MESSAGE(r.ok, r.message);

    // The avatar landed beside students.json with its exact bytes.
    char buf[64];
    size_t n = mocksd_read_file("/students/photos/A.jpg", buf, sizeof(buf) - 1);
    buf[n] = '\0';
    TEST_ASSERT_EQUAL_STRING("AVATAR-A-BYTES", buf);

    // Device-produced snapshots are untouched.
    n = mocksd_read_file("/students/checkins/A/2026-07-28_CS101_01.jpg", buf, sizeof(buf) - 1);
    buf[n] = '\0';
    TEST_ASSERT_EQUAL_STRING("CHECKIN-A", buf);
    n = mocksd_read_file("/photos/snap.jpg", buf, sizeof(buf) - 1);
    buf[n] = '\0';
    TEST_ASSERT_EQUAL_STRING("DEVICE-SNAP", buf);
}

static void test_revert_restores_previous(void) {
    // Chains from the import above: live is NEW, /backup/previous holds OLD.
    import_result_t r = import_service_revert();
    TEST_ASSERT_TRUE_MESSAGE(r.ok, r.message);
    TEST_ASSERT_EQUAL(1, teacher_count());  // back to the single old teacher
    device_config_t c;
    config_get(&c);
    TEST_ASSERT_EQUAL_STRING("Old Prof", c.teachers[0].name);
}

static void test_import_rejects_zip_slip_leaving_live_intact(void) {
    setup_live();
    TarBuf t;
    t.add("config.json", NEW_CONFIG);
    t.add("../evil.json", "pwned");  // path-escape entry
    t.end();
    t.stage_at("/config.tar");

    import_result_t r = import_service_run("/config.tar");
    TEST_ASSERT_FALSE(r.ok);
    TEST_ASSERT_NOT_NULL(strstr(r.message, "Invalid archive"));

    // Caught before backup or apply: live untouched, nothing sentinelled, no backup.
    TEST_ASSERT_EQUAL(1, teacher_count());
    TEST_ASSERT_TRUE(import_service_pending());  // source NOT renamed
    TEST_ASSERT_FALSE(mocksd_exists("/backup/previous/config.json"));
}

static void test_import_rejects_invalid_config_leaving_live_intact(void) {
    setup_live();
    // Structurally valid tar, but its config has two teachers sharing a password.
    TarBuf t;
    t.add("config.json",
          "{ \"teachers\": [ { \"name\": \"X\", \"email\": \"x\", \"password\": \"9\" }, "
          "{ \"name\": \"Y\", \"email\": \"y\", \"password\": \"9\" } ] }");
    t.add("students/students.json", NEW_STUDENTS);
    t.add("classes/CS101-M1/class.json", NEW_CLASS);
    t.end();
    t.stage_at("/config.tar");

    import_result_t r = import_service_run("/config.tar");
    TEST_ASSERT_FALSE(r.ok);
    TEST_ASSERT_NOT_NULL(strstr(r.message, "Config invalid"));

    // Validation runs on staging BEFORE apply, so the live config is intact.
    TEST_ASSERT_EQUAL(1, teacher_count());
    device_config_t c;
    config_get(&c);
    TEST_ASSERT_EQUAL_STRING("Old Prof", c.teachers[0].name);
    TEST_ASSERT_TRUE(import_service_pending());  // not sentinelled
}

int main(int, char**) {
    mock_freertos_set_delay_scale(1000);
    event_bus_init();
    UNITY_BEGIN();
    RUN_TEST(test_import_applies_and_backs_up);
    RUN_TEST(test_revert_restores_previous);  // chains from the import above
    RUN_TEST(test_import_rejects_zip_slip_leaving_live_intact);
    RUN_TEST(test_import_rejects_invalid_config_leaving_live_intact);
    RUN_TEST(test_import_applies_avatars_and_preserves_device_photos);
    RUN_TEST(test_stale_class_from_previous_config_does_not_hide_new_classes);
    return UNITY_END();
}
