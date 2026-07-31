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
    TEST_ASSERT_EQUAL_UINT8(100, battery_mv_to_pct(4300));  // above 4.0 V clamps
    TEST_ASSERT_EQUAL_UINT8(100, battery_mv_to_pct(4000));
    TEST_ASSERT_EQUAL_UINT8(0, battery_mv_to_pct(2700));  // 2.7 V is the 0% anchor
    TEST_ASSERT_EQUAL_UINT8(0, battery_mv_to_pct(2500));  // below clamps
}

static void test_battery_table_points(void) {
    // Linear 2.7 V (0%) .. 3.75 V (100%), anchors every 5% (~52 mV).
    TEST_ASSERT_EQUAL_UINT8(50, battery_mv_to_pct(3225));  // midpoint anchor
    TEST_ASSERT_EQUAL_UINT8(75, battery_mv_to_pct(3488));
    TEST_ASSERT_EQUAL_UINT8(25, battery_mv_to_pct(2963));
    TEST_ASSERT_EQUAL_UINT8(60, battery_mv_to_pct(3330));
}

// The board has no charge-status line, so "charging" is inferred from the rail
// reading higher than the pack alone can drive it.
static void test_battery_charging_threshold(void) {
    TEST_ASSERT_FALSE(battery_is_charging(3990));  // exactly 3.99 V is not yet charging
    TEST_ASSERT_TRUE(battery_is_charging(3991));
    TEST_ASSERT_TRUE(battery_is_charging(4200));  // on the charger
}

static void test_battery_full_pack_is_not_charging(void) {
    // A full battery sits at the curve's 100% anchor, well under the threshold:
    // the icon must not claim to be charging just because the pack is full.
    TEST_ASSERT_FALSE(battery_is_charging(3750));
    TEST_ASSERT_FALSE(battery_is_charging(3000));
    TEST_ASSERT_FALSE(battery_is_charging(0));
}

static void test_battery_interpolates_between_points(void) {
    // 3250 mV is 25/53 of the way from the 50% anchor (3225) to the 55% anchor
    // (3278): 52.36 -> rounds to 52.
    TEST_ASSERT_EQUAL_UINT8(52, battery_mv_to_pct(3250));
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
