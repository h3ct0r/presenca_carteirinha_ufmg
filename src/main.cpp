#include <Arduino.h>
#include <Wire.h>
#include <lvgl.h>

#include "audio/beeper.h"
#include "face_detection/face_app.h"
#include "lcd/st7701_lcd.h"
#include "lvgl_port.h"
#include "rfid/pn532_reader.h"
#include "touch/gt911_touch.h"

// Touch controller pins. This board wires no reset/backlight GPIO for the
// display panel itself (handled internally by the ST7701 driver), so only the
// touch controller pins are configured here.
#define TP_I2C_SDA 7
#define TP_I2C_SCL 8
#define TP_RST 3
#define TP_INT -1

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

// Runs in the RFID task, not the LVGL thread: log here, and hand anything
// UI-facing to lv_async_call.
static void on_card_detected(const uint8_t* uid, uint8_t uid_len) {
    char buf[3 * 7 + 1] = {0};
    for (uint8_t i = 0; i < uid_len && i < 7; i++) {
        snprintf(buf + i * 3, sizeof(buf) - i * 3, "%02X ", uid[i]);
    }
    Serial.printf("RFID card UID: %s\n", buf);
    face_app_notify_rfid(uid, uid_len);
    beeper_beep();
}

void setup() {
    Serial.begin(115200);
    Serial.println("ESP32-P4 face detection starting");

    i2c_bus_init();
    lcd.begin();
    touch.begin();

    lvgl_port_init(lcd, touch);

    // Builds the UI and starts the camera + face-detection task. Camera
    // hardware is optional: without it the face tab shows an error state.
    face_app_start();

    // Confirmation beep path (ES8311 -> NS4150 -> CN1). Optional: without
    // the codec answering, beeper_beep() is a no-op.
    beeper_init();

    // PN532 on CN3 (shared I2C bus). Optional: boot continues without it.
    pn532_reader_start(on_card_detected);
}

void loop() {
    lv_timer_handler();
    delay(5);
}
