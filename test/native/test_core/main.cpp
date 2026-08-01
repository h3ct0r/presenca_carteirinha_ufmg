// Pure-logic units: UID normalization, session, battery curve.

#include <stdio.h>
#include <string.h>
#include <unity.h>

#include "app/battery_curve.h"
#include "app/session.h"
#include "app/uid.h"

void setUp(void) {}
void tearDown(void) {}

// ---- uid_normalize ---------------------------------------------------------

static void test_uid_strips_separators_and_uppercases(void) {
    char out[32];
    uid_normalize("e0:d1-33 5f", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("E0D1335F", out);
}

static void test_uid_plain_passthrough(void) {
    char out[32];
    uid_normalize("04A31B2C", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("04A31B2C", out);
}

static void test_uid_empty_input(void) {
    char out[8] = "x";
    uid_normalize("", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("", out);
}

static void test_uid_truncates_to_capacity(void) {
    char out[4];
    uid_normalize("AA:BB:CC:DD", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("AAB", out);  // 3 chars + NUL
}

// ---- session ---------------------------------------------------------------

static void test_session_starts_logged_out(void) {
    TEST_ASSERT_FALSE(session_active());
    TEST_ASSERT_NULL(session_get());
}

static void test_session_set_copies_teacher(void) {
    teacher_t t = {};
    snprintf(t.name, sizeof(t.name), "Prof Test");
    snprintf(t.email, sizeof(t.email), "prof@test.edu");
    snprintf(t.rfid_uid, sizeof(t.rfid_uid), "E0:D1:33:5F");
    session_set(&t);

    // Mutating the source must not affect the stored session (copy, not ref).
    snprintf(t.name, sizeof(t.name), "MUTATED");

    TEST_ASSERT_TRUE(session_active());
    TEST_ASSERT_NOT_NULL(session_get());
    TEST_ASSERT_EQUAL_STRING("Prof Test", session_get()->name);
    TEST_ASSERT_EQUAL_STRING("prof@test.edu", session_get()->email);
}

static void test_session_clear(void) {
    teacher_t t = {};
    snprintf(t.name, sizeof(t.name), "Prof Test");
    session_set(&t);
    session_set(nullptr);
    TEST_ASSERT_FALSE(session_active());
    TEST_ASSERT_NULL(session_get());
}

// ---- battery curve ---------------------------------------------------------

static void test_battery_endpoints(void) {
    TEST_ASSERT_EQUAL_UINT8(100, battery_mv_to_pct(4300));  // above 100% anchor clamps
    TEST_ASSERT_EQUAL_UINT8(100, battery_mv_to_pct(4200));  // 4.20 V = 100%
    TEST_ASSERT_EQUAL_UINT8(0, battery_mv_to_pct(2700));    // 2.70 V = 0%
    TEST_ASSERT_EQUAL_UINT8(0, battery_mv_to_pct(2500));    // below clamps
}

static void test_battery_table_points(void) {
    // Linear 4.20 V (100%) .. 2.70 V (0%), anchors every 5% (75 mV).
    TEST_ASSERT_EQUAL_UINT8(50, battery_mv_to_pct(3450));
    TEST_ASSERT_EQUAL_UINT8(75, battery_mv_to_pct(3825));
    TEST_ASSERT_EQUAL_UINT8(25, battery_mv_to_pct(3075));
    TEST_ASSERT_EQUAL_UINT8(60, battery_mv_to_pct(3600));
}

// The board has no charge-status line, so "charging" is inferred from the rail
// reading strictly above the 4.20 V full anchor.
static void test_battery_charging_threshold(void) {
    TEST_ASSERT_FALSE(battery_is_charging(4200));  // exactly full is not charging
    TEST_ASSERT_TRUE(battery_is_charging(4201));
    TEST_ASSERT_TRUE(battery_is_charging(4300));   // USB topping
}

static void test_battery_full_pack_is_not_charging(void) {
    // A full pack at the 100% anchor must not show the charging icon.
    TEST_ASSERT_FALSE(battery_is_charging(4200));
    TEST_ASSERT_FALSE(battery_is_charging(3000));
    TEST_ASSERT_FALSE(battery_is_charging(0));
}

static void test_battery_interpolates_between_points(void) {
    // 3400 mV sits between 3375 (45%) and 3450 (50%):
    // 25/75 of the span → 45 + 1.67 → rounds to 47.
    TEST_ASSERT_EQUAL_UINT8(47, battery_mv_to_pct(3400));
}

static void test_battery_monotonic(void) {
    uint8_t prev = 0;
    for (uint16_t mv = 2900; mv <= 4300; mv += 10) {
        uint8_t pct = battery_mv_to_pct(mv);
        TEST_ASSERT_TRUE(pct >= prev);
        prev = pct;
    }
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_uid_strips_separators_and_uppercases);
    RUN_TEST(test_uid_plain_passthrough);
    RUN_TEST(test_uid_empty_input);
    RUN_TEST(test_uid_truncates_to_capacity);
    RUN_TEST(test_session_starts_logged_out);
    RUN_TEST(test_session_set_copies_teacher);
    RUN_TEST(test_session_clear);
    RUN_TEST(test_battery_endpoints);
    RUN_TEST(test_battery_table_points);
    RUN_TEST(test_battery_interpolates_between_points);
    RUN_TEST(test_battery_monotonic);
    RUN_TEST(test_battery_charging_threshold);
    RUN_TEST(test_battery_full_pack_is_not_charging);
    return UNITY_END();
}
