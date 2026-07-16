#ifndef _LVGL_PORT_H
#define _LVGL_PORT_H

#include "lcd/st7701_lcd.h"
#include "touch/gt911_touch.h"

// Initializes LVGL and binds it to the display and touch drivers:
//   - installs a millis()-based tick source,
//   - creates the display with two PSRAM partial-render buffers,
//   - registers the flush (display -> panel) and touch-read callbacks.
//
// Call once after lcd.begin() and touch.begin(). The referenced drivers must
// outlive the LVGL runtime, so pass long-lived (e.g. static) objects.
void lvgl_port_init(st7701_lcd& lcd, gt911_touch& touch);

#endif  // _LVGL_PORT_H
