#pragma once

#include <lvgl.h>

// Identifiers for every screen the device has. Also indexes the screen
// manager's registry, so keep SCREEN_COUNT last.
typedef enum : uint8_t {
    // Student-facing (dark kiosk look).
    SCREEN_IDLE,
    // Staff area (light look, ported from the legacy project).
    SCREEN_CLASSES,      // class list
    SCREEN_CLASS,        // one class: roll call / history / enroll tabs
    SCREEN_CLASS_STATS,  // one class: statistics + per-class settings
    SCREEN_ADMIN,        // admin panel: profile, SD usage, password, debug
    SCREEN_KIOSK,         // unattended student self check-in
    SCREEN_EXPORT,        // per-class CSV attendance export
    SCREEN_WIFI_EDITOR,   // debug WiFi soft-AP + (future) file editor
    SCREEN_CAMERA,        // camera preview + face detection
    SCREEN_ABOUT,         // project info, credits, version, repo QR
    SCREEN_COUNT,
} screen_id_t;

// The interface every screen implements. All hooks run on the LVGL thread.
typedef struct {
    // Builds the widget tree once (lazy, on first show) and returns its root
    // (an lv_obj_create(NULL) screen object). The tree stays alive across
    // hide/show; per-visit state belongs in on_show.
    lv_obj_t* (*create)(void);
    // (Re)binds data and starts timers/animations. arg is only valid during
    // the call — copy what you need. May be NULL.
    void (*on_show)(void* arg);
    // Stops timers and releases heavy resources (e.g. SD-decoded images).
    void (*on_hide)(void);
} screen_t;
