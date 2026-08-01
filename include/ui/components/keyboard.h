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
//
// The textarea releases the keyboard on its own LV_EVENT_DELETE, so deleting one
// (lv_obj_clean(), screen teardown) while the keyboard points at it is safe —
// LVGL does not clear lv_keyboard_t::ta itself, and the next
// lv_keyboard_set_textarea() would otherwise touch freed memory. Calling
// keyboard_hide() first is still good practice for the visible behaviour, but it
// is no longer what stands between the UI and a use-after-free.
lv_obj_t* keyboard_make_textarea(lv_obj_t* parent, const char* placeholder,
                                 uint32_t max_len, lv_keyboard_mode_t mode);

// True while the shared keyboard is on screen.
bool keyboard_is_visible(void);

// Fired when the keyboard appears or disappears. The keyboard floats over the
// UI on lv_layer_top(), so screens normally reserve its height as bottom
// padding — this lets them reserve it only while it is actually up, instead of
// leaving dead space when it is not. Pass nullptr to clear; a screen MUST clear
// it when it tears down, or the callback fires against a destroyed layout.
typedef void (*keyboard_visibility_cb_t)(bool visible);
void keyboard_set_visibility_cb(keyboard_visibility_cb_t cb);
