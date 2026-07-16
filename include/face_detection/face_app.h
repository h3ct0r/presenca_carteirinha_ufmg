#ifndef _FACE_APP_H
#define _FACE_APP_H

#include <stdint.h>

// Builds the tabview UI (Face Detect tab + Widgets tab) and starts the
// camera + detection task. Call once from setup() after lv_init and the
// display/touch are registered. Camera hardware is optional: without it the
// tab shows the error state.
void face_app_start();

// Shows the card UID (hex) on the Face Detect tab. Thread-safe: callable
// from the RFID task; the LVGL-side refresh timer picks it up.
void face_app_notify_rfid(const uint8_t* uid, uint8_t uid_len);

#endif
