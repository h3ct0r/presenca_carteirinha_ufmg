#pragma once

#include "ui/screen.h"

// Staff: one class loaded from the SD card. A view stack, not a tab bar — a hub
// with two big actions opens into a dated roll-call session or the past-session
// history, and enrollment is reached from within an open session.
//
// on_show arg: const class_rec_t* from roster_service (its static storage,
// so the pointer stays valid while shown). Pass NULL to re-show the last
// class.
extern const screen_t scr_class;

// Makes the next scr_class on_show land on the open-session view instead of the
// class hub. Used when returning from kiosk mode so the professor drops straight
// back into the running roll call. LVGL thread; call just before scr_mgr_show.
void scr_class_request_session_view(void);
