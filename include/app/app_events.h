#pragma once

#include <stddef.h>
#include <stdint.h>

// The event vocabulary shared by every layer. Services post these to the
// event bus from their own tasks; the LVGL thread drains them in loop().
// Events are plain PODs copied by value into the queue, so payloads must be
// fixed-size (no pointers to task-local buffers).
typedef enum : uint8_t {
    APP_EVENT_CARD_SCANNED,  // payload: card
    APP_EVENT_NET_STATE,     // payload: net
    APP_EVENT_POWER_STATE,   // payload: power
    APP_EVENT_CONFIG_STATE,  // payload: config
    APP_EVENT_ROSTER_STATE,  // payload: roster
} app_event_type_t;

typedef struct {
    app_event_type_t type;
    union {
        struct {
            uint8_t uid[10];  // ISO14443A UIDs are 4/7/10 bytes
            uint8_t uid_len;
        } card;
        struct {
            bool connected;
            int8_t rssi;  // dBm, meaningful only while connected
        } net;
        struct {
            uint8_t pct;   // 0..100
            uint16_t mv;   // battery voltage, millivolts
        } power;
        struct {
            uint8_t status;  // config_status_t (see services/config_service.h)
        } config;
        struct {
            uint8_t status;  // roster_status_t (see services/roster_service.h)
        } roster;
    };
} app_event_t;

// Formats a card UID as "AA:BB:CC:DD" into buf. Needs 3 bytes per UID byte;
// truncates safely if buf is smaller.
static inline void app_event_uid_to_hex(const uint8_t* uid, uint8_t uid_len, char* buf,
                                        size_t buf_size) {
    size_t pos = 0;
    for (uint8_t i = 0; i < uid_len && pos + 3 < buf_size; i++) {
        const char* hex = "0123456789ABCDEF";
        if (i) buf[pos++] = ':';
        buf[pos++] = hex[uid[i] >> 4];
        buf[pos++] = hex[uid[i] & 0x0F];
    }
    if (buf_size) buf[pos] = '\0';
}
