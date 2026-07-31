// The flush path pushes every rendered pixel, so keep this translation unit at
// -O3 even though the project builds at -Os.
#pragma GCC optimize("O3")

#include "lvgl_port.h"

#include <Arduino.h>
#include <assert.h>
#include <lvgl.h>

#include "esp_heap_caps.h"
#include "esp_lcd_mipi_dsi.h"

// Panel geometry (portrait). The LVGL display and its render buffers are sized
// from these.
static constexpr int LCD_H_RES = 480;
static constexpr int LCD_V_RES = 800;
static constexpr int LCD_BYTES_PER_PIXEL = 2;  // RGB565

// Touch sampling period, independent of the 33 ms display refresh — see the
// note in lvgl_port_init().
static constexpr uint32_t TOUCH_READ_PERIOD_MS = 10;
// Scroll momentum decay, percent lost per throw step (LVGL default 10).
static constexpr uint8_t SCROLL_THROW_PCT = 20;

// The drivers are owned by the caller; we keep references only so the LVGL
// callbacks (which carry no user context of their own) can reach them.
static st7701_lcd* s_lcd = nullptr;
static gt911_touch* s_touch = nullptr;

// Signals LVGL that the panel finished transmitting a flushed region. Runs in
// the LCD ISR context, so it must stay allocation- and log-free.
static bool on_color_trans_done(esp_lcd_panel_handle_t panel,
                                esp_lcd_dpi_panel_event_data_t* edata, void* user_ctx) {
    lv_display_flush_ready((lv_display_t*)user_ctx);
    return false;
}

// LVGL v9 flush callback: pushes a rendered region to the panel. LVGL hands the
// pixels as raw bytes; the ST7701 driver consumes them as RGB565 words. The
// draw area is inclusive, so the end coordinates are bumped by one.
static void flush_cb(lv_display_t* disp, const lv_area_t* area, uint8_t* px_map) {
    s_lcd->lcd_draw_bitmap(area->x1, area->y1, area->x2 + 1, area->y2 + 1, (uint16_t*)px_map);
}

// LVGL v9 input callback: reports the latest touch point, or a release.
static void touch_read_cb(lv_indev_t* indev, lv_indev_data_t* data) {
    uint16_t x, y;
    if (s_touch->getTouch(&x, &y)) {
        data->state = LV_INDEV_STATE_PRESSED;
        data->point.x = x;
        data->point.y = y;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

void lvgl_port_init(st7701_lcd& lcd, gt911_touch& touch) {
    s_lcd = &lcd;
    s_touch = &touch;

    lv_init();

    // LVGL v9 has no built-in clock: without a tick source every LVGL timer
    // (refresh, input, animations) sees 0ms elapsed and never fires.
    lv_tick_set_cb([]() -> uint32_t { return millis(); });

    lv_display_t* disp = lv_display_create(LCD_H_RES, LCD_V_RES);

    // Two full-frame buffers in PSRAM enable partial rendering with flush
    // overlap (LVGL renders into one while the other is transmitted).
    const size_t buf_bytes = (size_t)LCD_H_RES * LCD_V_RES * LCD_BYTES_PER_PIXEL;
    uint8_t* buf1 = (uint8_t*)heap_caps_malloc(buf_bytes, MALLOC_CAP_SPIRAM);
    uint8_t* buf2 = (uint8_t*)heap_caps_malloc(buf_bytes, MALLOC_CAP_SPIRAM);
    assert(buf1 && buf2);

    lv_display_set_buffers(disp, buf1, buf2, buf_bytes, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(disp, flush_cb);

    // Route the panel's transfer-done event back into LVGL so flushes pipeline.
    bsp_lcd_handles_t panels;
    lcd.get_handle(&panels);
    esp_lcd_dpi_panel_event_callbacks_t cbs = {};
    cbs.on_color_trans_done = on_color_trans_done;
    esp_lcd_dpi_panel_register_event_callbacks(panels.panel, &cbs, disp);

    lv_indev_t* indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, touch_read_cb);

    // Sample the touch panel faster than the display refreshes. lv_indev_create()
    // gives the read timer LV_DEF_REFR_PERIOD (33 ms), which is too coarse twice
    // over: the list follows the finger in ~20 px steps during a fast drag, and
    // LVGL estimates the flick from a time-decayed sum of the last 8 per-sample
    // deltas that ignores anything older than 99 ms — at 33 ms only ~3 of those 8
    // slots are ever in the window, so how far a flick throws depends on where
    // the release happened to land between samples. At 10 ms the window is full
    // of small deltas and the throw becomes repeatable. Costs one extra I2C read
    // per period on the shared bus.
    lv_timer_set_period(lv_indev_get_read_timer(indev), TOUCH_READ_PERIOD_MS);

    // Momentum decay per throw step, in percent (LVGL's default is 10, i.e. it
    // keeps 90% of the velocity each step, which glides well past where the
    // finger stopped). Note this is per animation STEP, not per millisecond, so
    // it is tied to the refresh period: changing LV_DEF_REFR_PERIOD changes how
    // long the glide lasts in wall time.
    lv_indev_set_scroll_throw(indev, SCROLL_THROW_PCT);
}
