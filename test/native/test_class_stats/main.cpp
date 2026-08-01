// class_stats: the turma breakdown shown on the class and class-stats screens.
// Both screens used to carry their own copy of this loop; this is the shared one.

#include <stdio.h>
#include <string.h>
#include <unity.h>

#include "app/class_stats.h"

void setUp(void) {}
void tearDown(void) {}

// Builds a class whose roster carries the given turma tags in order.
static class_rec_t make_class(const char* const* turmas, int n) {
    class_rec_t c = {};
    c.roster_count = n;
    for (int i = 0; i < n; i++) {
        c.roster[i] = (int16_t)i;
        snprintf(c.roster_turma[i], sizeof(c.roster_turma[i]), "%s", turmas[i]);
    }
    return c;
}

static void test_single_turma(void) {
    const char* t[] = {"TE1", "TE1", "TE1"};
    class_rec_t c = make_class(t, 3);
    char out[160];
    TEST_ASSERT_TRUE(class_turma_breakdown(&c, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("TE1: 3", out);
}

// Groups keep the order they first appear in the roster, so the display is
// stable rather than depending on a sort.
static void test_several_turmas_in_first_seen_order(void) {
    const char* t[] = {"TE2", "TE1", "TE2", "TE1", "TE2"};
    class_rec_t c = make_class(t, 5);
    char out[160];
    TEST_ASSERT_TRUE(class_turma_breakdown(&c, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("TE2: 3    TE1: 2", out);
}

// A class that does not use turmas returns false so the caller hides the line —
// but the counts are still formatted, under "(none)".
static void test_untagged_only_returns_false(void) {
    const char* t[] = {"", "", ""};
    class_rec_t c = make_class(t, 3);
    char out[160];
    TEST_ASSERT_FALSE(class_turma_breakdown(&c, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("(none): 3", out);
}

static void test_mixed_tagged_and_untagged(void) {
    const char* t[] = {"M1", "", "M1", "", ""};
    class_rec_t c = make_class(t, 5);
    char out[160];
    TEST_ASSERT_TRUE(class_turma_breakdown(&c, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("M1: 2    (none): 3", out);
}

static void test_empty_roster(void) {
    class_rec_t c = {};
    char out[160] = "stale";
    TEST_ASSERT_FALSE(class_turma_breakdown(&c, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("", out);
}

// More distinct turmas than the table holds: the listed ones stay correct and
// the rest are summarised rather than silently dropped.
static void test_too_many_turmas_are_summarised(void) {
    char names[30][8];
    const char* t[30];
    for (int i = 0; i < 30; i++) {
        snprintf(names[i], sizeof(names[i]), "T%d", i);
        t[i] = names[i];
    }
    class_rec_t c = make_class(t, 30);
    char out[400];
    TEST_ASSERT_TRUE(class_turma_breakdown(&c, out, sizeof(out)));
    TEST_ASSERT_NOT_NULL(strstr(out, "T0: 1"));
    TEST_ASSERT_NOT_NULL(strstr(out, "T15: 1"));           // last that fits
    TEST_ASSERT_NULL(strstr(out, "T16: 1"));               // first that does not
    TEST_ASSERT_NOT_NULL(strstr(out, "+14 more"));         // 30 - 16
}

// A tiny buffer must truncate cleanly: NUL-terminated, no overrun, and the
// return value still reports whether turmas are in use.
static void test_small_buffer_truncates_safely(void) {
    const char* t[] = {"TE1", "TE1", "TE2"};
    class_rec_t c = make_class(t, 3);
    char guard[32];
    memset(guard, 0x7F, sizeof(guard));
    char* out = guard;
    TEST_ASSERT_TRUE(class_turma_breakdown(&c, out, 8));
    TEST_ASSERT_TRUE(strlen(out) < 8);
    TEST_ASSERT_EQUAL_HEX8(0x7F, (unsigned char)guard[8]);  // nothing past cap
}

static void test_null_inputs_are_safe(void) {
    char out[16] = "stale";
    TEST_ASSERT_FALSE(class_turma_breakdown(nullptr, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("", out);

    const char* t[] = {"A"};
    class_rec_t c = make_class(t, 1);
    TEST_ASSERT_FALSE(class_turma_breakdown(&c, nullptr, 16));
    TEST_ASSERT_FALSE(class_turma_breakdown(&c, out, 0));
}

// A full roster is the realistic worst case; it must not overrun the table.
static void test_full_roster_of_one_turma(void) {
    class_rec_t c = {};
    c.roster_count = ROSTER_MAX_CLASS_STUDENTS;
    for (int i = 0; i < ROSTER_MAX_CLASS_STUDENTS; i++) {
        snprintf(c.roster_turma[i], sizeof(c.roster_turma[i]), "TE1");
    }
    char out[160];
    TEST_ASSERT_TRUE(class_turma_breakdown(&c, out, sizeof(out)));
    // Derived, not literal: the roster above is sized from the cap, so the
    // expectation has to track it or this goes stale the next time it moves.
    char expect[32];
    snprintf(expect, sizeof(expect), "TE1: %d", ROSTER_MAX_CLASS_STUDENTS);
    TEST_ASSERT_EQUAL_STRING(expect, out);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_single_turma);
    RUN_TEST(test_several_turmas_in_first_seen_order);
    RUN_TEST(test_untagged_only_returns_false);
    RUN_TEST(test_mixed_tagged_and_untagged);
    RUN_TEST(test_empty_roster);
    RUN_TEST(test_too_many_turmas_are_summarised);
    RUN_TEST(test_small_buffer_truncates_safely);
    RUN_TEST(test_null_inputs_are_safe);
    RUN_TEST(test_full_roster_of_one_turma);
    return UNITY_END();
}
