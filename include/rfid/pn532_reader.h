#pragma once

#include <stdint.h>

// Callback invoked when a new card is presented. Runs in the RFID task
// context, NOT the LVGL thread: don't touch LVGL objects directly from it
// (use lv_async_call to hop onto the UI thread).
typedef void (*pn532_card_cb_t)(const uint8_t* uid, uint8_t uid_len);

// Initializes the PN532 over the shared I2C bus (Wire1, the ES_I2C bus on
// GPIO7/8 exposed by connector CN3) and starts a FreeRTOS task that polls
// for ISO14443A cards. Returns false if the PN532 doesn't answer (not
// connected / DIP switches not set to I2C mode).
bool pn532_reader_start(pn532_card_cb_t on_card);
