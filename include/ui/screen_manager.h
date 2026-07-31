#pragma once

#include "ui/screen.h"

// Owns navigation: which screen is visible and the transitions between them.
// Screens never call lv_screen_load themselves — they ask the manager, so
// on_show/on_hide always run in matched pairs. LVGL thread only.

// Registers a screen implementation. Call once per screen before the first
// scr_mgr_show(); the screen_t must be statically allocated.
void scr_mgr_register(screen_id_t id, const screen_t* screen);

// Shows a screen, creating it on first use. arg is forwarded to on_show and
// need only stay valid for the duration of the call. Showing the screen
// that's already visible re-runs on_hide/on_show to refresh its data, without
// a transition.
void scr_mgr_show(screen_id_t id, void* arg);

// The currently visible screen, or SCREEN_COUNT before the first show.
screen_id_t scr_mgr_current(void);
