#include "services/rfid_service.h"

#include <string.h>

#include "app/event_bus.h"
#include "rfid/pn532_reader.h"

// Runs in the RFID task: package the UID into an event and hand it to the
// LVGL thread. No LVGL calls, no lookups here — anything slow or UI-related
// belongs on the other side of the bus.
static void on_card(const uint8_t* uid, uint8_t uid_len) {
    app_event_t ev = {};
    ev.type = APP_EVENT_CARD_SCANNED;
    if (uid_len > sizeof(ev.card.uid)) uid_len = sizeof(ev.card.uid);
    memcpy(ev.card.uid, uid, uid_len);
    ev.card.uid_len = uid_len;
    event_bus_post(&ev);
}

bool rfid_service_start(void) {
    return pn532_reader_start(on_card);
}
