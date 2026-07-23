#include "ui/screen_manager.h"

#include "esp32-hal-log.h"

static const char* TAG = "scr_mgr";

static const screen_t* s_screens[SCREEN_COUNT] = {};
static lv_obj_t* s_roots[SCREEN_COUNT] = {};
static screen_id_t s_current = SCREEN_COUNT;

void scr_mgr_register(screen_id_t id, const screen_t* screen) {
    s_screens[id] = screen;
}

void scr_mgr_show(screen_id_t id, void* arg) {
    const screen_t* scr = (id < SCREEN_COUNT) ? s_screens[id] : nullptr;
    if (!scr) {
        ESP_LOGE(TAG, "screen %u not registered", (unsigned)id);
        return;
    }

    // Refresh-in-place when the screen is already visible.
    if (id == s_current) {
        if (scr->on_hide) scr->on_hide();
        if (scr->on_show) scr->on_show(arg);
        return;
    }

    if (!s_roots[id]) s_roots[id] = scr->create();

    if (s_current < SCREEN_COUNT && s_screens[s_current]->on_hide) {
        s_screens[s_current]->on_hide();
    }

    // on_show runs before the transition so the content is already bound
    // when the first frame of the fade renders.
    if (scr->on_show) scr->on_show(arg);
    s_current = id;
    lv_screen_load_anim(s_roots[id], LV_SCR_LOAD_ANIM_FADE_IN, 150, 0, false);
}

screen_id_t scr_mgr_current(void) {
    return s_current;
}
