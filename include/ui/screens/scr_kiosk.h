#pragma once

#include "ui/screen.h"

// Unattended student self check-in. Students register their presence for the
// currently open attendance session by tapping their RFID card or typing
// their (numeric) university ID; the input is validated against the class
// roster. Reached from the class screen's kiosk button, which is enabled only
// while a session is open.
//
// on_show arg: const class_rec_t* (the class whose session is open).
extern const screen_t scr_kiosk;
