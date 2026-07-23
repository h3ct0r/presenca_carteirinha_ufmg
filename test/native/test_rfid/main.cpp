// RFID service against the fake PN532 reader: "reading a tag" is mocked by
// mock_pn532_tap(), which fires the same callback path the real reader task
// uses; the service must translate it into a CARD_SCANNED event.

#include <string.h>
#include <unity.h>

#include "app/event_bus.h"
#include "mock_pn532.h"
#include "services/rfid_service.h"

void setUp(void) {
    app_event_t ev;
    while (event_bus_poll(&ev)) {
    }
}
void tearDown(void) {}

static void test_start_registers_with_reader(void) {
    mock_pn532_set_start_result(true);
    TEST_ASSERT_TRUE(rfid_service_start());
    TEST_ASSERT_TRUE(mock_pn532_started());
}

static void test_tap_posts_card_scanned_event(void) {
    const uint8_t uid[7] = {0xE0, 0xD1, 0x33, 0x5F, 0x01, 0x02, 0x03};
    mock_pn532_tap(uid, 7);

    app_event_t ev = {};
    TEST_ASSERT_TRUE(event_bus_poll(&ev));
    TEST_ASSERT_EQUAL(APP_EVENT_CARD_SCANNED, ev.type);
    TEST_ASSERT_EQUAL_UINT8(7, ev.card.uid_len);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(uid, ev.card.uid, 7);
}

static void test_oversized_uid_is_clamped(void) {
    // Event payload holds at most 10 bytes; a longer UID must be truncated,
    // not overflow.
    const uint8_t uid[12] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
    mock_pn532_tap(uid, 12);

    app_event_t ev = {};
    TEST_ASSERT_TRUE(event_bus_poll(&ev));
    TEST_ASSERT_EQUAL_UINT8(10, ev.card.uid_len);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(uid, ev.card.uid, 10);
}

static void test_start_fails_when_reader_absent(void) {
    mock_pn532_set_start_result(false);
    TEST_ASSERT_FALSE(rfid_service_start());
}

int main(int, char**) {
    event_bus_init();
    UNITY_BEGIN();
    RUN_TEST(test_start_registers_with_reader);
    RUN_TEST(test_tap_posts_card_scanned_event);
    RUN_TEST(test_oversized_uid_is_clamped);
    RUN_TEST(test_start_fails_when_reader_absent);
    return UNITY_END();
}
