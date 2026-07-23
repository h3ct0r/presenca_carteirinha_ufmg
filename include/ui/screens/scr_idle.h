#pragma once

#include "ui/screen.h"

// Idle / attract screen and the device's access gate: invites the authorized
// user to tap their card (or enter the password), and signals a missing/
// invalid SD config. No on_show argument.
extern const screen_t scr_idle;

// Feeds a scanned card UID to the gate while the idle screen is showing.
// Called from the UI event handler; grants access (opens the app) on the
// authorized tag, shows a denied hint otherwise. No-op if config isn't valid.
void scr_idle_handle_scan(const char* uid_hex);
