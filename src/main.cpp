#include <Arduino.h>
#include <Wire.h>
#include <lvgl.h>

#include "app/event_bus.h"
#include "audio/beeper.h"
#include "lcd/st7701_lcd.h"
#include "lvgl_port.h"
#include "services/battery_service.h"
#include "services/config_service.h"
#include "services/rfid_service.h"
#include "services/roster_service.h"
#include "touch/gt911_touch.h"
#include "ui/ui.h"

// The whole UI — including the camera preview, which renders a large 480x270
// image and drains events on the Arduino loop (LVGL) task — runs on loopTask.
// The core's default 8 KB stack overflows in that draw path (Guru Meditation:
// "loopTask" stack protection fault on the camera screen). Give it headroom.
SET_LOOP_TASK_STACK_SIZE(16 * 1024);

// Touch controller pins. This board wires no reset/backlight GPIO for the
// display panel itself (handled internally by the ST7701 driver), so only the
// touch controller pins are configured here.
#define TP_I2C_SDA 7
#define TP_I2C_SCL 8
#define TP_RST 3
#define TP_INT -1

LV_FONT_DECLARE(font_montserrat_custom_14);

// Display panel has no external reset line on this board.
static st7701_lcd lcd(-1);
static gt911_touch touch(TP_I2C_SDA, TP_I2C_SCL, TP_RST, TP_INT);

// Brings up the shared I2C master bus (port 1) used by the touch controller,
// camera sensor and PN532 RFID reader (connector CN3, same ES_I2C net).
// Wire1 maps to I2C port 1 and, on this Arduino core (3.x / IDF 5.5), is
// implemented on top of the same i2c_master driver the IDF-side consumers
// use: touch and camera keep fetching the bus with
// i2c_master_get_bus_handle(1), while the Adafruit PN532 library talks
// through Wire1 directly. One bus, two APIs, one owner.
static void i2c_bus_init() {
    Wire1.begin(TP_I2C_SDA, TP_I2C_SCL, 100000);
}

// Central event dispatch, always on the LVGL thread. The attendance
// controller will hook in here before the UI once it lands.
static void app_dispatch(const app_event_t& ev) {
    ui_handle_event(&ev);
}

void setup() {
    Serial.begin(115200);
    Serial.println("ESP32-P4 face detection starting");

    event_bus_init();

    i2c_bus_init();
    lcd.begin();
    touch.begin();

    // Confirmation beep path (ES8311 -> NS4150 -> CN1). Optional: without
    // the codec answering, beeper_beep() is a no-op.
    beeper_init();

    // Load /config.json and the student/class data before the UI so the
    // idle screen's first frame already reflects the real SD state.
    config_service_start();
    roster_service_start();

    lvgl_port_init(lcd, touch);
    ui_init();

    if (!rfid_service_start()) {
        Serial.println("RFID unavailable, continuing without it");
    }

    battery_service_start();
}

void loop() {
    app_event_t ev;
    while (event_bus_poll(&ev)) {
        app_dispatch(ev);
    }
    lv_timer_handler();
    delay(5);
}
