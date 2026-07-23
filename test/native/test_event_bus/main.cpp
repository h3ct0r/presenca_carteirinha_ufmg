// Event bus over the mock FreeRTOS queue: FIFO order, overflow drop policy.

#include <string.h>
#include <unity.h>

#include "app/event_bus.h"

void setUp(void) {
    // Drain anything a previous test left behind.
    app_event_t ev;
    while (event_bus_poll(&ev)) {
    }
}
void tearDown(void) {}

static app_event_t card_event(uint8_t first_byte) {
    app_event_t ev = {};
    ev.type = APP_EVENT_CARD_SCANNED;
    ev.card.uid[0] = first_byte;
    ev.card.uid_len = 4;
    return ev;
}

static void test_post_then_poll_roundtrip(void) {
    app_event_t ev = card_event(0xAB);
    TEST_ASSERT_TRUE(event_bus_post(&ev));

    app_event_t out = {};
    TEST_ASSERT_TRUE(event_bus_poll(&out));
    TEST_ASSERT_EQUAL(APP_EVENT_CARD_SCANNED, out.type);
    TEST_ASSERT_EQUAL_UINT8(0xAB, out.card.uid[0]);
    TEST_ASSERT_EQUAL_UINT8(4, out.card.uid_len);

    TEST_ASSERT_FALSE(event_bus_poll(&out));  // empty again
}

static void test_fifo_order(void) {
    for (uint8_t i = 1; i <= 3; i++) {
        app_event_t ev = card_event(i);
        TEST_ASSERT_TRUE(event_bus_post(&ev));
    }
    app_event_t out;
    for (uint8_t i = 1; i <= 3; i++) {
        TEST_ASSERT_TRUE(event_bus_poll(&out));
        TEST_ASSERT_EQUAL_UINT8(i, out.card.uid[0]);
    }
}

static void test_overflow_drops_new_events(void) {
    // Queue is 16 deep; the 17th post must be rejected, not block.
    for (int i = 0; i < 16; i++) {
        app_event_t ev = card_event((uint8_t)i);
        TEST_ASSERT_TRUE(event_bus_post(&ev));
    }
    app_event_t extra = card_event(0xFF);
    TEST_ASSERT_FALSE(event_bus_post(&extra));

    int drained = 0;
    app_event_t out;
    while (event_bus_poll(&out)) drained++;
    TEST_ASSERT_EQUAL_INT(16, drained);
}

int main(int, char**) {
    event_bus_init();
    UNITY_BEGIN();
    RUN_TEST(test_post_then_poll_roundtrip);
    RUN_TEST(test_fifo_order);
    RUN_TEST(test_overflow_drops_new_events);
    return UNITY_END();
}
