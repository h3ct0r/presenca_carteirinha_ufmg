// card_gate: turning a stream of PN532 poll results into tap decisions.
//
// The bug this exists for: with two cards on the reader, the PN532 returns
// whichever wins anticollision, and that alternates poll to poll. The old
// "is it the same as last time?" debounce therefore fired on every poll and
// checked both students in repeatedly.

#include <string.h>
#include <unity.h>

#include "app/card_gate.h"

void setUp(void) { card_gate_reset(); }
void tearDown(void) {}

static const uint8_t A[4] = {0xAA, 0x01, 0x02, 0x03};
static const uint8_t B[4] = {0xBB, 0x04, 0x05, 0x06};
static const uint8_t C[7] = {0xCC, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C};

// One poll returning `uid`.
static card_gate_action_t poll(const uint8_t* uid, uint8_t len) {
    uint8_t out[10];
    uint8_t out_len = 0;
    return card_gate_poll(uid, len, out, &out_len);
}

// One poll with an empty field.
static card_gate_action_t poll_empty(void) { return card_gate_poll(nullptr, 0, nullptr, nullptr); }

// --- one card ---------------------------------------------------------------

static void test_single_card_is_accepted_once_confirmed(void) {
    // First sighting is not enough — it could be one of two alternating cards.
    TEST_ASSERT_EQUAL_INT(CARD_GATE_IGNORE, poll(A, 4));
    TEST_ASSERT_EQUAL_INT(CARD_GATE_ACCEPT, poll(A, 4));
}

static void test_accept_reports_the_uid(void) {
    uint8_t out[10] = {0};
    uint8_t out_len = 0;
    card_gate_poll(C, 7, out, &out_len);
    TEST_ASSERT_EQUAL_INT(CARD_GATE_ACCEPT, card_gate_poll(C, 7, out, &out_len));
    TEST_ASSERT_EQUAL_INT(7, out_len);
    TEST_ASSERT_EQUAL_MEMORY(C, out, 7);
}

static void test_held_card_is_accepted_only_once(void) {
    poll(A, 4);
    TEST_ASSERT_EQUAL_INT(CARD_GATE_ACCEPT, poll(A, 4));
    for (int i = 0; i < 30; i++) {
        TEST_ASSERT_EQUAL_INT(CARD_GATE_IGNORE, poll(A, 4));
    }
}

static void test_lifting_and_re_presenting_registers_again(void) {
    poll(A, 4);
    TEST_ASSERT_EQUAL_INT(CARD_GATE_ACCEPT, poll(A, 4));
    poll_empty();
    poll(A, 4);
    TEST_ASSERT_EQUAL_INT(CARD_GATE_ACCEPT, poll(A, 4));
}

// --- two cards: the reported bug --------------------------------------------

// The exact failing sequence: two cards on the reader, the PN532 flip-flopping.
// Neither may be registered, however long it goes on.
static void test_alternating_cards_are_never_accepted(void) {
    int accepts = 0;
    for (int i = 0; i < 40; i++) {
        if (poll(i % 2 ? B : A, 4) == CARD_GATE_ACCEPT) accepts++;
    }
    TEST_ASSERT_EQUAL_INT(0, accepts);
}

// The device must say something rather than appear dead — but only once, not
// on every poll.
static void test_alternation_reports_one_collision(void) {
    int collisions = 0;
    for (int i = 0; i < 40; i++) {
        if (poll(i % 2 ? B : A, 4) == CARD_GATE_COLLISION) collisions++;
    }
    TEST_ASSERT_EQUAL_INT(1, collisions);
}

// The collision is only detectable once a UID repeats after a different one,
// which is the third poll of an A,B,A sequence.
static void test_collision_is_reported_on_the_repeat(void) {
    TEST_ASSERT_EQUAL_INT(CARD_GATE_IGNORE, poll(A, 4));
    TEST_ASSERT_EQUAL_INT(CARD_GATE_IGNORE, poll(B, 4));
    TEST_ASSERT_EQUAL_INT(CARD_GATE_COLLISION, poll(A, 4));
}

// Even a card that manages a confirmed run is refused while the reader is
// known to hold several: the operator must present one card alone.
static void test_no_card_is_accepted_after_a_collision(void) {
    poll(A, 4);
    poll(B, 4);
    TEST_ASSERT_EQUAL_INT(CARD_GATE_COLLISION, poll(A, 4));
    for (int i = 0; i < 10; i++) {
        TEST_ASSERT_EQUAL_INT(CARD_GATE_IGNORE, poll(B, 4));
    }
}

// Taking both cards off and presenting one clears the jam.
static void test_clearing_the_field_recovers_from_a_collision(void) {
    poll(A, 4);
    poll(B, 4);
    TEST_ASSERT_EQUAL_INT(CARD_GATE_COLLISION, poll(A, 4));
    poll_empty();
    poll(A, 4);
    TEST_ASSERT_EQUAL_INT(CARD_GATE_ACCEPT, poll(A, 4));
}

static void test_three_cards_also_collide_and_register_nobody(void) {
    const uint8_t* seq[] = {A, B, C, A, B, C, A, B, C};
    const uint8_t lens[] = {4, 4, 7, 4, 4, 7, 4, 4, 7};
    int accepts = 0, collisions = 0;
    for (unsigned i = 0; i < sizeof(lens); i++) {
        card_gate_action_t a = poll(seq[i], lens[i]);
        if (a == CARD_GATE_ACCEPT) accepts++;
        if (a == CARD_GATE_COLLISION) collisions++;
    }
    TEST_ASSERT_EQUAL_INT(0, accepts);
    TEST_ASSERT_EQUAL_INT(1, collisions);
}

// --- queue behaviour --------------------------------------------------------

// One student lifts and the next taps so fast the field is never seen empty.
// That is two separate taps, not a collision — B must register.
static void test_back_to_back_students_both_register(void) {
    poll(A, 4);
    TEST_ASSERT_EQUAL_INT(CARD_GATE_ACCEPT, poll(A, 4));
    TEST_ASSERT_EQUAL_INT(CARD_GATE_IGNORE, poll(B, 4));   // first sighting of B
    TEST_ASSERT_EQUAL_INT(CARD_GATE_ACCEPT, poll(B, 4));   // confirmed
}

// A single dropped read (the card is momentarily out of range) must not be
// mistaken for a lift-and-retap or a second card.
static void test_a_dropped_read_does_not_double_register(void) {
    poll(A, 4);
    TEST_ASSERT_EQUAL_INT(CARD_GATE_ACCEPT, poll(A, 4));
    poll_empty();  // one missed poll — treated as a genuine lift
    poll(A, 4);
    TEST_ASSERT_EQUAL_INT(CARD_GATE_ACCEPT, poll(A, 4));  // deliberate: re-tap counts
}

// --- degenerate input -------------------------------------------------------

static void test_empty_field_is_always_ignored(void) {
    for (int i = 0; i < 5; i++) TEST_ASSERT_EQUAL_INT(CARD_GATE_IGNORE, poll_empty());
}

static void test_null_uid_is_treated_as_empty(void) {
    poll(A, 4);
    TEST_ASSERT_EQUAL_INT(CARD_GATE_IGNORE, card_gate_poll(nullptr, 4, nullptr, nullptr));
    // The reset means A has to confirm again from scratch.
    poll(A, 4);
    TEST_ASSERT_EQUAL_INT(CARD_GATE_ACCEPT, poll(A, 4));
}

static void test_uids_of_different_lengths_are_distinct(void) {
    // A 4-byte UID must not compare equal to a 7-byte one sharing its prefix.
    const uint8_t shortA[4] = {0xCC, 0x07, 0x08, 0x09};
    poll(shortA, 4);
    TEST_ASSERT_EQUAL_INT(CARD_GATE_ACCEPT, poll(shortA, 4));
    TEST_ASSERT_EQUAL_INT(CARD_GATE_IGNORE, poll(C, 7));
    TEST_ASSERT_EQUAL_INT(CARD_GATE_ACCEPT, poll(C, 7));
}

static void test_reset_clears_everything(void) {
    poll(A, 4);
    poll(A, 4);
    card_gate_reset();
    poll(A, 4);
    TEST_ASSERT_EQUAL_INT(CARD_GATE_ACCEPT, poll(A, 4));
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_single_card_is_accepted_once_confirmed);
    RUN_TEST(test_accept_reports_the_uid);
    RUN_TEST(test_held_card_is_accepted_only_once);
    RUN_TEST(test_lifting_and_re_presenting_registers_again);
    RUN_TEST(test_alternating_cards_are_never_accepted);
    RUN_TEST(test_alternation_reports_one_collision);
    RUN_TEST(test_collision_is_reported_on_the_repeat);
    RUN_TEST(test_no_card_is_accepted_after_a_collision);
    RUN_TEST(test_clearing_the_field_recovers_from_a_collision);
    RUN_TEST(test_three_cards_also_collide_and_register_nobody);
    RUN_TEST(test_back_to_back_students_both_register);
    RUN_TEST(test_a_dropped_read_does_not_double_register);
    RUN_TEST(test_empty_field_is_always_ignored);
    RUN_TEST(test_null_uid_is_treated_as_empty);
    RUN_TEST(test_uids_of_different_lengths_are_distinct);
    RUN_TEST(test_reset_clears_everything);
    return UNITY_END();
}
