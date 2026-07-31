// battery_log against the in-memory SD card: CSV header on first write,
// append-only rows after that.
//
// The module is always compiled (and so always tested); BATTERY_DRAIN_LOG only
// gates the call site in main.cpp.

#include <stdio.h>
#include <string.h>
#include <unity.h>

#include "mock_sd.h"
#include "storage/battery_log.h"

void setUp(void) {}
void tearDown(void) {}

static const char* PATH = "/battery.csv";

static void read_log(char* buf, size_t cap) {
    size_t n = mocksd_read_file(PATH, buf, cap - 1);
    buf[n] = '\0';
}

static void test_path_is_at_the_card_root(void) {
    TEST_ASSERT_EQUAL_STRING(PATH, battery_log_path());
}

static void test_first_sample_writes_the_header(void) {
    mocksd_reset();
    TEST_ASSERT_TRUE(battery_log_append(10, 4148, 98));

    char buf[128];
    read_log(buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("uptime_s,mv,pct\n10,4148,98\n", buf);
}

static void test_later_samples_append_without_a_second_header(void) {
    mocksd_reset();
    battery_log_append(10, 4148, 98);
    battery_log_append(20, 4147, 98);
    battery_log_append(30, 4140, 97);

    char buf[256];
    read_log(buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING(
        "uptime_s,mv,pct\n"
        "10,4148,98\n"
        "20,4147,98\n"
        "30,4140,97\n",
        buf);
}

// A reboot restarts the uptime clock; the log just keeps appending, which is
// why the header documents uptime as per-run rather than continuous.
static void test_a_reboot_appends_to_the_same_file(void) {
    mocksd_reset();
    battery_log_append(3600, 3900, 60);
    battery_log_append(10, 3890, 59);  // uptime went backwards: new run

    char buf[256];
    read_log(buf, sizeof(buf));
    TEST_ASSERT_NOT_NULL(strstr(buf, "3600,3900,60\n10,3890,59\n"));
    // Still exactly one header.
    const char* first = strstr(buf, "uptime_s,mv,pct");
    TEST_ASSERT_NOT_NULL(first);
    TEST_ASSERT_NULL(strstr(first + 1, "uptime_s,mv,pct"));
}

// A long drain test runs for hours: the values must not wrap or truncate.
static void test_wide_values_are_recorded_intact(void) {
    mocksd_reset();
    battery_log_append(359999, 4200, 100);  // ~100 h uptime, full cell
    battery_log_append(360009, 3000, 0);    // empty

    char buf[256];
    read_log(buf, sizeof(buf));
    TEST_ASSERT_NOT_NULL(strstr(buf, "359999,4200,100\n"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "360009,3000,0\n"));
}

static void test_every_row_is_one_line(void) {
    mocksd_reset();
    for (int i = 0; i < 20; i++) battery_log_append((uint32_t)(i * 10), (uint16_t)(4200 - i), 90);

    char buf[512];
    read_log(buf, sizeof(buf));
    int lines = 0;
    for (const char* p = buf; *p; p++) {
        if (*p == '\n') lines++;
    }
    TEST_ASSERT_EQUAL_INT(21, lines);  // 20 samples + the header
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_path_is_at_the_card_root);
    RUN_TEST(test_first_sample_writes_the_header);
    RUN_TEST(test_later_samples_append_without_a_second_header);
    RUN_TEST(test_a_reboot_appends_to_the_same_file);
    RUN_TEST(test_wide_values_are_recorded_intact);
    RUN_TEST(test_every_row_is_one_line);
    return UNITY_END();
}
