#include "ui/components/keyboard.h"

#include "audio/beeper.h"
#include "ui/theme/theme.h"

static lv_obj_t* s_kbd = nullptr;
static lv_event_cb_t s_ready_cb = nullptr;  // optional OK/finish action for the current field

// Reduced numeric keypad (digits + backspace + OK): drops the default number
// map's "+/-", ".", arrows and text-switch button. Passwords and IDs are
// digits-only, and text fields can't reach NUMBER mode, so this is safe global.
static const char* const NUM_MAP[] = {"1", "2", "3", "\n",
                                      "4", "5", "6", "\n",
                                      "7", "8", "9", "\n",
                                      LV_SYMBOL_BACKSPACE, "0", LV_SYMBOL_OK, ""};
#define W1 ((lv_buttonmatrix_ctrl_t)1)  // relative width 1, no flags
static const lv_buttonmatrix_ctrl_t NUM_CTRL[] = {W1, W1, W1, W1, W1, W1,
                                                  W1, W1, W1, W1, W1, W1};
#undef W1

static void kbd_event_cb(lv_event_t* e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY) {
        if (s_ready_cb) {
            s_ready_cb(e);  // e.g. submit the password to unlock (cb closes the flow)
        } else {
            keyboard_hide();
        }
    } else if (code == LV_EVENT_CANCEL) {
        keyboard_hide();
    }
}

// Keyboards don't go through ui_add_press_feedback, so beep each key here.
static void kbd_key_beep_cb(lv_event_t*) { beeper_touch(); }

void keyboard_show(lv_obj_t* ta, lv_keyboard_mode_t mode) {
    lv_keyboard_set_textarea(s_kbd, ta);
    lv_keyboard_set_mode(s_kbd, mode);
    // The reduced numeric pad has few big keys, so 32 px reads well; the full
    // QWERTY has ~10 keys per row and needs a smaller face to fit them.
    const lv_font_t* face =
        (mode == LV_KEYBOARD_MODE_NUMBER) ? &lv_font_montserrat_32 : &lv_font_montserrat_20;
    lv_obj_set_style_text_font(s_kbd, face, LV_PART_ITEMS);
    lv_obj_remove_flag(s_kbd, LV_OBJ_FLAG_HIDDEN);
}

static void textarea_focus_cb(lv_event_t* e) {
    lv_obj_t* ta = (lv_obj_t*)lv_event_get_target(e);
    keyboard_show(ta, (lv_keyboard_mode_t)(uintptr_t)lv_event_get_user_data(e));
}

void keyboard_create(void) {
    s_kbd = lv_keyboard_create(lv_layer_top());
    lv_obj_set_size(s_kbd, LV_PCT(100), 300);
    lv_obj_align(s_kbd, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_flag(s_kbd, LV_OBJ_FLAG_HIDDEN);
    lv_keyboard_set_map(s_kbd, LV_KEYBOARD_MODE_NUMBER, NUM_MAP, NUM_CTRL);
    // Bigger key labels — the default 14px is small for the numeric keypad.
    lv_obj_set_style_text_font(s_kbd, &lv_font_montserrat_32, LV_PART_ITEMS);
    lv_obj_add_event_cb(s_kbd, kbd_event_cb, LV_EVENT_ALL, nullptr);
    lv_obj_add_event_cb(s_kbd, kbd_key_beep_cb, LV_EVENT_VALUE_CHANGED, nullptr);
}

void keyboard_hide(void) {
    s_ready_cb = nullptr;
    lv_keyboard_set_textarea(s_kbd, nullptr);
    lv_obj_add_flag(s_kbd, LV_OBJ_FLAG_HIDDEN);
}

void keyboard_set_ready_cb(lv_event_cb_t cb) { s_ready_cb = cb; }

lv_obj_t* keyboard_make_textarea(lv_obj_t* parent, const char* placeholder,
                                 uint32_t max_len, lv_keyboard_mode_t mode) {
    lv_obj_t* ta = lv_textarea_create(parent);
    lv_obj_remove_style_all(ta);
    lv_obj_add_style(ta, &theme_style_input, 0);
    lv_obj_set_style_text_color(ta, lv_color_hex(THEME_MUTED), LV_PART_TEXTAREA_PLACEHOLDER);
    lv_obj_set_width(ta, LV_PCT(100));
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_max_length(ta, max_len);
    lv_textarea_set_placeholder_text(ta, placeholder);
    // Keep the focused field visible above the keyboard overlay.
    lv_obj_add_flag(ta, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_obj_add_event_cb(ta, textarea_focus_cb, LV_EVENT_FOCUSED, (void*)(uintptr_t)mode);
    lv_obj_add_event_cb(ta, textarea_focus_cb, LV_EVENT_CLICKED, (void*)(uintptr_t)mode);
    return ta;
}
