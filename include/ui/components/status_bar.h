#pragma once

// Persistent top bar with the WiFi and battery indicators. Lives on
// lv_layer_top(), so it stays visible across every screen change without any
// screen knowing about it. Binds to the ui_state subjects at creation and
// updates itself from then on.
//
// Call once from ui_init(), after ui_state_init(). LVGL thread only.
void status_bar_create(void);

// Height in px, for screens that want to keep content clear of the overlay.
constexpr int STATUS_BAR_HEIGHT = 36;
