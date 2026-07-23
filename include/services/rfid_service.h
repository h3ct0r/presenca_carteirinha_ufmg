#pragma once

// Starts the RFID service: brings up the PN532 reader (which runs its own
// polling task, see rfid/pn532_reader.h) and forwards each detected card to
// the event bus as APP_EVENT_CARD_SCANNED.
//
// Requires event_bus_init() and the I2C bus (Wire1) to be up. Returns false
// if the PN532 doesn't answer; the rest of the device keeps working.
bool rfid_service_start(void);
