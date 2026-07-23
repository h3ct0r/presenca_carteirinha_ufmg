#pragma once

#include "ui/screen.h"

// Staff: one class loaded from the SD card, with tabs for today's roll call,
// past-session history, and student enrollment. Presence marking/history
// arrive with the attendance feature; until then Today shows the roster
// unchecked and History is an empty state.
//
// on_show arg: const class_rec_t* from roster_service (its static storage,
// so the pointer stays valid while shown). Pass NULL to re-show the last
// class.
extern const screen_t scr_class;
