#pragma once

#include "app/app_events.h"

// The UI layer's only public surface. main.cpp calls these two functions and
// nothing else UI-related; which screens exist and how they react to events
// stays inside ui/.

// Registers all screens and shows the idle screen. Call once, after
// lvgl_port_init(). LVGL thread only.
void ui_init(void);

// Reacts to an app event (navigation, data refresh). Called from the event
// drain loop, i.e. always on the LVGL thread.
void ui_handle_event(const app_event_t* ev);

// Diverts the next scanned card UID to cb instead of the idle access gate,
// for flows that need to read a card (e.g. enrolling a student). One-shot:
// cleared automatically after it fires. Pass NULL to cancel a pending
// capture. LVGL thread only.
typedef void (*ui_card_cb_t)(const char* uid_hex);
void ui_set_card_capture(ui_card_cb_t cb);
