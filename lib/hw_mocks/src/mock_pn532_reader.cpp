#include "mock_pn532.h"

// Mirrors include/rfid/pn532_reader.h (kept dependency-free on purpose;
// signatures must stay in sync with that header).
typedef void (*pn532_card_cb_t)(const uint8_t* uid, uint8_t uid_len);
typedef void (*pn532_collision_cb_t)(void);

static pn532_card_cb_t s_cb = nullptr;
static pn532_collision_cb_t s_collision_cb = nullptr;
static bool s_start_ok = true;
static bool s_started = false;

bool pn532_reader_start(pn532_card_cb_t on_card, pn532_collision_cb_t on_collision) {
    s_cb = on_card;
    s_collision_cb = on_collision;
    if (!s_start_ok) return false;
    s_started = true;
    return true;
}

void mock_pn532_set_start_result(bool ok) { s_start_ok = ok; }

bool mock_pn532_started(void) { return s_started; }

void mock_pn532_tap(const uint8_t* uid, uint8_t uid_len) {
    if (s_cb) s_cb(uid, uid_len);
}

void mock_pn532_collision(void) {
    if (s_collision_cb) s_collision_cb();
}
