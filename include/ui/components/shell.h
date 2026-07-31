#pragma once

#include <lvgl.h>

#include "ui/screen.h"  // screen_id_t

// The staff screens' common chrome: gradient header with title and subtitle,
// scrollable body, and an optional footer nav (Classes / Export / WiFi File
// Editor / Admin). The whole column is padded below the global status bar
// overlay.
typedef struct {
    lv_obj_t* root;      // pass to the screen manager
    lv_obj_t* header;    // the top bar row (for shell_set_back)
    lv_obj_t* body;      // scrollable flex column; put content here
    lv_obj_t* title;     // header title label, updatable in on_show
    lv_obj_t* subtitle;  // header subtitle label
    lv_obj_t* footer;    // bottom nav row, or NULL when created without one
} shell_t;

shell_t shell_create(const char* title, const char* subtitle, bool with_footer);

// Adds a leading back button to the header. Use on detail screens (which have
// no footer) so there is a clear way back. cb runs on tap.
void shell_set_back(shell_t* sh, lv_event_cb_t cb);

// Enables/disables the bottom nav buttons (dimmed + non-clickable when off).
// Used to trap the user on a screen until they finish something (e.g. stop the
// WiFi AP). No-op when the shell has no footer.
void shell_set_nav_enabled(shell_t* sh, bool enabled);

// Highlights the bottom-nav button for `id` (the current screen) and resets the
// others, so the active tab is visually distinct. Call once after shell_create.
void shell_set_active_nav(shell_t* sh, screen_id_t id);

// Adds a compact icon action button to the right side of the header (after
// the title). `font` may be NULL for the default font. Pass a non-NULL `label`
// to append a text word after the icon (the button grows to fit); NULL keeps
// the fixed 44 px icon-only square. Returns the button; its icon label is
// child 0 (update it with lv_label_set_text on that child).
lv_obj_t* shell_add_action(shell_t* sh, const char* text, const lv_font_t* font,
                           lv_event_cb_t cb, const char* label = nullptr);
