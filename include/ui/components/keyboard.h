#pragma once

#include <lvgl.h>

// One shared on-screen keyboard for the whole UI, living on lv_layer_top()
// (the legacy project created one keyboard per screen shell). Textareas made
// with keyboard_make_textarea() summon it on focus; READY/CANCEL dismiss it.
//
// Screens with forms must call keyboard_hide() from their on_hide hook so
// the keyboard never outlives its screen. LVGL thread only.

// Call once from ui_init().
void keyboard_create(void);

void keyboard_hide(void);

// Summons the shared keyboard for `ta` in `mode` (what focusing a textarea
// does). Use to pop the keyboard up automatically, without a tap.
void keyboard_show(lv_obj_t* ta, lv_keyboard_mode_t mode);

// Sets a one-shot action for the keyboard's OK/finish button (LV_EVENT_READY);
// pass nullptr to restore the default (just hide). Cleared on keyboard_hide(),
// so set it after keyboard_show() and it lasts until the keyboard is dismissed.
void keyboard_set_ready_cb(lv_event_cb_t cb);

// Creates a themed one-line textarea wired to the shared keyboard.
lv_obj_t* keyboard_make_textarea(lv_obj_t* parent, const char* placeholder,
                                 uint32_t max_len, lv_keyboard_mode_t mode);
