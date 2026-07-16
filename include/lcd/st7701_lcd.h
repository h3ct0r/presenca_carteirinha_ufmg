#ifndef _ST7701_LCD_H
#define _ST7701_LCD_H

#include <stdint.h>

#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_types.h"

// Driver handles produced during panel bring-up. Handed out so the LVGL/DSI
// glue can register its flush-done callback on the DPI panel.
typedef struct {
    esp_lcd_dsi_bus_handle_t mipi_dsi_bus;  // MIPI DSI bus handle
    esp_lcd_panel_io_handle_t io;           // ESP LCD IO handle
    esp_lcd_panel_handle_t panel;           // ESP LCD panel (color) handle
    esp_lcd_panel_handle_t control;         // ESP LCD panel (control) handle
} bsp_lcd_handles_t;

// Thin C++ wrapper around Espressif's ST7701 MIPI-DSI panel driver for the
// Guition JC4880P443C 480x800 display.
class st7701_lcd {
public:
    // lcd_rst: external panel reset GPIO, or -1 if the board has none.
    explicit st7701_lcd(int8_t lcd_rst);

    // Powers the DSI PHY, initializes the panel, and turns the backlight on.
    void begin();

    // Blits an RGB565 region to the panel. Coordinates are half-open:
    // [x_start, x_end) x [y_start, y_end), matching esp_lcd_panel_draw_bitmap.
    void lcd_draw_bitmap(uint16_t x_start, uint16_t y_start, uint16_t x_end, uint16_t y_end,
                         uint16_t* color_data);

    uint16_t width() const;
    uint16_t height() const;

    // Copies the underlying driver handles out (used to wire up LVGL flush).
    void get_handle(bsp_lcd_handles_t* ret_handles) const;

private:
    void enable_dsi_phy_power();
    void init_backlight();
    void set_backlight(uint32_t level);

    int8_t _lcd_rst;
};

#endif  // _ST7701_LCD_H
