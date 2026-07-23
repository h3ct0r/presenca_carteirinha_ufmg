#pragma once

#include "ui/screen.h"

// Confirmation screen shown after a card tap. Displays who was recognized
// (name/photo will come from the SD student DB later; for now the UID) and
// returns to SCREEN_IDLE on its own after a few seconds.

// on_show argument. Copied by the screen, so it may live on the caller's
// stack.
typedef struct {
    char uid_hex[32];  // "AA:BB:CC:DD..."
} scr_student_arg_t;

extern const screen_t scr_student;
