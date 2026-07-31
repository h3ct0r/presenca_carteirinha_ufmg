#pragma once

#include "app/teacher.h"

// The currently logged-in professor. Set on a successful unlock, cleared on
// sign-out. Touched only from the LVGL thread (login in the idle screen,
// Sign Out in the admin screen, reads in the UI), so no locking is needed.

// Copies t as the active session; NULL clears it (logged out).
void session_set(const teacher_t* t);

// The logged-in professor, or NULL if nobody is logged in.
const teacher_t* session_get(void);

bool session_active(void);
