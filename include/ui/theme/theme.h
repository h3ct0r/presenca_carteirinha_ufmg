#pragma once

#include <lvgl.h>

// The design system: every color and shared style lives here, nowhere else.
//
// Two palettes coexist on purpose: the dark "kiosk" look for the
// student-facing screens (idle / confirmation / status bar) and the light
// "staff" look for the teacher screens, ported from the legacy
// sistema-carteirinha project. Legacy constants that were byte-swapped to
// compensate for that board's BGR panel (e.g. danger was stored as blue
// 0x4545D6) are normalized to true RGB here.

// Light (staff) palette.
constexpr uint32_t THEME_BG = 0xEAF1F4;
constexpr uint32_t THEME_SURFACE = 0xFFFFFF;
constexpr uint32_t THEME_PRIMARY = 0x272766;       // indigo
constexpr uint32_t THEME_PRIMARY_DARK = 0x0D1F4A;  // navy
constexpr uint32_t THEME_ACCENT = 0x129CF3;        // sky blue
constexpr uint32_t THEME_TEXT = 0x1B1F23;
constexpr uint32_t THEME_MUTED = 0x6B7280;
constexpr uint32_t THEME_BORDER = 0xD4D8DD;
constexpr uint32_t THEME_SOFT = 0xE9EEF5;  // input/list-row fill
constexpr uint32_t THEME_SUCCESS = 0x1F9D55;
constexpr uint32_t THEME_SUCCESS_SOFT = 0xDFF6E7;
constexpr uint32_t THEME_DANGER = 0xD64545;
constexpr uint32_t THEME_WARNING = 0xE8890C;       // amber — info/alert accents
constexpr uint32_t THEME_WARNING_SOFT = 0xFFEFD6;  // light orange callout fill
constexpr uint32_t THEME_ON_PRIMARY = 0xFFFFFF;
constexpr uint32_t THEME_HEADER_SUBTITLE = 0xB8C0DC;

// Dark (kiosk) palette.
constexpr uint32_t THEME_DARK_BG = 0x111827;
constexpr uint32_t THEME_DARK_BG_DEEP = 0x0B1220;
constexpr uint32_t THEME_DARK_TEXT = 0xF9FAFB;
constexpr uint32_t THEME_DARK_MUTED = 0x9CA3AF;
constexpr uint32_t THEME_DARK_ACCENT = 0x38BDF8;
constexpr uint32_t THEME_DARK_OK = 0x22C55E;
constexpr uint32_t THEME_DARK_WARN = 0xF87171;

// Standard button height across the whole app (double the old ~36 px
// content-sized height, for comfortable touch targets). Every button made
// with ui_make_button() gets this height; screens should not override it.
constexpr int UI_BUTTON_HEIGHT = 72;

// Shared styles, initialized once by theme_init(). Add to widgets with
// lv_obj_add_style(); never re-create these per screen.
extern lv_style_t theme_style_card;
extern lv_style_t theme_style_btn_primary;
extern lv_style_t theme_style_btn_outline;
extern lv_style_t theme_style_btn_accent;
extern lv_style_t theme_style_btn_danger;
extern lv_style_t theme_style_input;
extern lv_style_t theme_style_tab_active;
extern lv_style_t theme_style_tab_idle;
// Applied on LV_STATE_PRESSED for consistent tap feedback (shrink + dim).
extern lv_style_t theme_style_pressed;

// Call once from ui_init(), before any screen is created.
void theme_init(void);

// Small widget factories shared by the staff screens.
lv_obj_t* ui_make_label(lv_obj_t* parent, const char* text, uint32_t color,
                        const lv_font_t* font);
lv_obj_t* ui_make_button(lv_obj_t* parent, const char* text, lv_style_t* style,
                         lv_event_cb_t cb, void* user_data);

// Adds the standard press feedback (shrink + dim on LV_STATE_PRESSED) to any
// clickable object. ui_make_button() already calls this; use it for other
// actionable items (cards, custom buttons) so feedback stays consistent.
void ui_add_press_feedback(lv_obj_t* obj);
lv_obj_t* ui_make_card(lv_obj_t* parent);  // surface card, 100% wide, auto height
