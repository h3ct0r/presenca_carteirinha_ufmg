// app/ustar — the on-device reader for the config-builder's ustar archive.
// Builds archives byte-for-byte like tools/config-builder/src/tarball.js and
// checks parsing, the §4 name whitelist, and the error paths.

#include <stdio.h>
#include <string.h>
#include <unity.h>

#include "app/ustar.h"

void setUp(void) {}
void tearDown(void) {}

// --- archive builder (mirrors tarball.js) -----------------------------------

static void put(uint8_t* h, const char* s, int off, int len) {
    for (int i = 0; i < len && s[i]; i++) h[off + i] = (uint8_t)s[i];
}

// Writes a 512-byte header for `name`/`size`/`typeflag` at h, with a valid
// checksum. Returns nothing; header is exactly one block.
static void build_header(uint8_t h[512], const char* name, size_t size, char typeflag) {
    memset(h, 0, 512);
    put(h, name, 0, 100);
    put(h, "0000644", 100, 7);
    put(h, "0000000", 108, 7);
    put(h, "0000000", 116, 7);
    char so[16];
    snprintf(so, sizeof(so), "%011lo", (unsigned long)size);
    put(h, so, 124, 11);
    put(h, "00000000000", 136, 11);  // mtime 0 (no RTC)
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

// A growable archive builder over a fixed scratch buffer.
struct TarBuf {
    uint8_t b[8192];
    size_t len = 0;
    void add(const char* name, const char* data, char typeflag = '0') {
        size_t size = data ? strlen(data) : 0;
        build_header(b + len, name, size, typeflag);
        len += 512;
        if (size) {
            memcpy(b + len, data, size);
            len += ((size + 511) / 512) * 512;
        }
    }
    void end() {  // two zero blocks
        memset(b + len, 0, 1024);
        len += 1024;
    }
};

// --- visitor collecting entries ---------------------------------------------

struct Seen {
    int count = 0;
    char names[8][64];
    size_t sizes[8];
    char first_byte[8];
    int abort_after = -1;  // return false once count reaches this (−1 = never)
};

static bool collect(const ustar_entry_t* e, void* ctx) {
    Seen* s = (Seen*)ctx;
    if (s->count < 8) {
        snprintf(s->names[s->count], sizeof(s->names[s->count]), "%s", e->name);
        s->sizes[s->count] = e->size;
        s->first_byte[s->count] = e->size ? (char)e->data[0] : '\0';
    }
    s->count++;
    if (s->abort_after >= 0 && s->count >= s->abort_after) return false;
    return true;
}

// --- tests ------------------------------------------------------------------

static void test_single_file(void) {
    TarBuf t;
    t.add("config.json", "{\"x\":1}");
    t.end();
    Seen s;
    TEST_ASSERT_EQUAL(USTAR_OK, ustar_iterate(t.b, t.len, 0, collect, &s));
    TEST_ASSERT_EQUAL(1, s.count);
    TEST_ASSERT_EQUAL_STRING("config.json", s.names[0]);
    TEST_ASSERT_EQUAL_UINT(7, s.sizes[0]);
    TEST_ASSERT_EQUAL('{', s.first_byte[0]);
}

static void test_multiple_entries_in_order(void) {
    TarBuf t;
    t.add("config.json", "cfg");
    t.add("students/students.json", "stud");
    t.add("classes/CS101-M1/class.json", "cls");
    t.end();
    Seen s;
    TEST_ASSERT_EQUAL(USTAR_OK, ustar_iterate(t.b, t.len, 0, collect, &s));
    TEST_ASSERT_EQUAL(3, s.count);
    TEST_ASSERT_EQUAL_STRING("config.json", s.names[0]);
    TEST_ASSERT_EQUAL_STRING("students/students.json", s.names[1]);
    TEST_ASSERT_EQUAL_STRING("classes/CS101-M1/class.json", s.names[2]);
    TEST_ASSERT_EQUAL_UINT(4, s.sizes[1]);
}

static void test_empty_archive_is_ok(void) {
    TarBuf t;
    t.end();  // just the two zero blocks
    Seen s;
    TEST_ASSERT_EQUAL(USTAR_OK, ustar_iterate(t.b, t.len, 0, collect, &s));
    TEST_ASSERT_EQUAL(0, s.count);
}

static void test_zip_slip_dotdot_rejected(void) {
    TarBuf t;
    t.add("../evil.json", "x");
    t.end();
    Seen s;
    TEST_ASSERT_EQUAL(USTAR_ERR_NAME, ustar_iterate(t.b, t.len, 0, collect, &s));
    TEST_ASSERT_EQUAL(0, s.count);  // aborted before any visit
}

static void test_absolute_path_rejected(void) {
    TarBuf t;
    t.add("/etc/passwd", "x");
    t.end();
    Seen s;
    TEST_ASSERT_EQUAL(USTAR_ERR_NAME, ustar_iterate(t.b, t.len, 0, collect, &s));
}

static void test_non_whitelisted_file_rejected(void) {
    TarBuf t;
    t.add("classes/CS101-M1/attendance/2026-07-26.jsonl", "x");
    t.end();
    Seen s;
    TEST_ASSERT_EQUAL(USTAR_ERR_NAME, ustar_iterate(t.b, t.len, 0, collect, &s));
}

static void test_bad_magic_rejected(void) {
    TarBuf t;
    t.add("config.json", "x");
    t.b[257] = 'X';  // corrupt the magic (checksum no longer matters — magic checked first)
    t.end();
    Seen s;
    TEST_ASSERT_EQUAL(USTAR_ERR_MAGIC, ustar_iterate(t.b, t.len, 0, collect, &s));
}

static void test_bad_checksum_rejected(void) {
    TarBuf t;
    t.add("config.json", "x");
    t.b[124] = '9';  // change the size field without recomputing checksum ('9' is non-octal too)
    t.b[125] = '9';
    t.end();
    Seen s;
    TEST_ASSERT_EQUAL(USTAR_ERR_CHECKSUM, ustar_iterate(t.b, t.len, 0, collect, &s));
}

static void test_truncated_payload_rejected(void) {
    TarBuf t;
    t.add("config.json", "hello world");
    t.end();
    // Cut the buffer to just past the header, losing the payload block.
    Seen s;
    TEST_ASSERT_EQUAL(USTAR_ERR_TRUNCATED, ustar_iterate(t.b, 512 + 4, 0, collect, &s));
}

static void test_oversize_rejected(void) {
    TarBuf t;
    t.add("config.json", "x");
    t.end();
    Seen s;
    TEST_ASSERT_EQUAL(USTAR_ERR_TOO_BIG, ustar_iterate(t.b, t.len, t.len - 1, collect, &s));
}

static void test_directory_entry_skipped(void) {
    TarBuf t;
    t.add("classes/CS101-M1/", nullptr, '5');  // directory entry
    t.add("classes/CS101-M1/class.json", "cls");
    t.end();
    Seen s;
    TEST_ASSERT_EQUAL(USTAR_OK, ustar_iterate(t.b, t.len, 0, collect, &s));
    TEST_ASSERT_EQUAL(1, s.count);  // only the file, not the directory
    TEST_ASSERT_EQUAL_STRING("classes/CS101-M1/class.json", s.names[0]);
}

static void test_visitor_abort(void) {
    TarBuf t;
    t.add("config.json", "a");
    t.add("students/students.json", "b");
    t.end();
    Seen s;
    s.abort_after = 1;  // stop after the first entry
    TEST_ASSERT_EQUAL(USTAR_ERR_ABORT, ustar_iterate(t.b, t.len, 0, collect, &s));
    TEST_ASSERT_EQUAL(1, s.count);
}

static void test_null_visitor_validates_only(void) {
    TarBuf good;
    good.add("config.json", "x");
    good.end();
    TEST_ASSERT_EQUAL(USTAR_OK, ustar_iterate(good.b, good.len, 0, nullptr, nullptr));

    TarBuf bad;
    bad.add("../evil", "x");
    bad.end();
    TEST_ASSERT_EQUAL(USTAR_ERR_NAME, ustar_iterate(bad.b, bad.len, 0, nullptr, nullptr));
}

static void test_name_allowed_predicate(void) {
    TEST_ASSERT_TRUE(ustar_name_allowed("config.json"));
    TEST_ASSERT_TRUE(ustar_name_allowed("students/students.json"));
    TEST_ASSERT_TRUE(ustar_name_allowed("classes/CS101-M1/class.json"));
    TEST_ASSERT_TRUE(ustar_name_allowed("classes/2026_2-DCC219/class.json"));
    TEST_ASSERT_TRUE(ustar_name_allowed("students/photos/2025115525.jpg"));
    TEST_ASSERT_TRUE(ustar_name_allowed("students/photos/abc.jpg"));

    TEST_ASSERT_FALSE(ustar_name_allowed("config.jsonx"));
    TEST_ASSERT_FALSE(ustar_name_allowed("students/other.json"));
    TEST_ASSERT_FALSE(ustar_name_allowed("classes/a/b/class.json"));  // nested segment
    TEST_ASSERT_FALSE(ustar_name_allowed("classes//class.json"));     // empty segment
    TEST_ASSERT_FALSE(ustar_name_allowed("classes/CS101/roster.json"));
    TEST_ASSERT_FALSE(ustar_name_allowed("/config.json"));            // absolute
    TEST_ASSERT_FALSE(ustar_name_allowed("classes/../class.json"));   // dotdot
    TEST_ASSERT_FALSE(ustar_name_allowed(""));

    // Avatar tree: a single .jpg segment only.
    TEST_ASSERT_FALSE(ustar_name_allowed("students/photos/"));          // no file
    TEST_ASSERT_FALSE(ustar_name_allowed("students/photos/.jpg"));      // empty stem
    TEST_ASSERT_FALSE(ustar_name_allowed("students/photos/a/b.jpg"));   // nested
    TEST_ASSERT_FALSE(ustar_name_allowed("students/photos/id.png"));    // wrong ext
    TEST_ASSERT_FALSE(ustar_name_allowed("students/photos/id.jpeg"));   // wrong ext
    TEST_ASSERT_FALSE(ustar_name_allowed("students/photos/../x.jpg"));  // dotdot
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_single_file);
    RUN_TEST(test_multiple_entries_in_order);
    RUN_TEST(test_empty_archive_is_ok);
    RUN_TEST(test_zip_slip_dotdot_rejected);
    RUN_TEST(test_absolute_path_rejected);
    RUN_TEST(test_non_whitelisted_file_rejected);
    RUN_TEST(test_bad_magic_rejected);
    RUN_TEST(test_bad_checksum_rejected);
    RUN_TEST(test_truncated_payload_rejected);
    RUN_TEST(test_oversize_rejected);
    RUN_TEST(test_directory_entry_skipped);
    RUN_TEST(test_visitor_abort);
    RUN_TEST(test_null_visitor_validates_only);
    RUN_TEST(test_name_allowed_predicate);
    return UNITY_END();
}
