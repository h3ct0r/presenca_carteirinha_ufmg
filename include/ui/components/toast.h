#pragma once

// A transient notification banner on lv_layer_top (above every screen).
// Green for success, red for failure. Dismisses itself after 5 seconds or as
// soon as it's tapped. Only one is shown at a time. LVGL thread only.
void ui_toast_show(const char* msg, bool ok);
