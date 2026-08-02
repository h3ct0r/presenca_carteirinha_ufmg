#pragma once

#include <stddef.h>
#include <stdint.h>

// Camera preview + (optional) face detection, running on its own FreeRTOS task
// off the LVGL thread. Owns the OV02C10 -> MIPI-CSI -> ISP -> PPA pipeline and
// publishes a thread-safe snapshot of the latest downscaled preview frame and
// the detected face boxes, which the camera screen polls on the LVGL thread.
//
// Face inference (ESP-DL human_face_detect) is compiled in only when the build
// defines USE_FACE_DETECT and the esp-dl framework is available; without it the
// preview still runs, just with no boxes. See docs/software/FACE_DETECTION.md.
//
// Layering: this is a service (owns hardware, runs a task). It never touches
// LVGL — the UI lives in ui/screens/scr_camera.

constexpr int FACE_PREVIEW_W = 480;
constexpr int FACE_PREVIEW_H = 270;
constexpr int FACE_MAX_BOXES = 5;

// Saved size of a "Take picture" snapshot. Exactly half the sensor's 1920x1080
// in each axis, so the PPA scale is a clean 2:1 and the 16:9 framing is
// unchanged. Quarter the pixels of the full-res frame it replaces: a JPEG lands
// around 100-150 KB instead of 300-500, and the .bmp fallback 1.5 MB instead
// of 6.
constexpr int SNAPSHOT_W = 960;
constexpr int SNAPSHOT_H = 540;

typedef struct {
    int x, y, w, h;  // in preview pixels (FACE_PREVIEW_W x FACE_PREVIEW_H)
    float score;
} face_box_t;

// Brings up the camera pipeline and starts the detection task. Idempotent:
// safe to call on every screen entry. Returns false if the camera failed to
// initialize (no sensor / CSI error); the rest of the API stays safe either
// way (snapshots just never arrive).
bool face_detection_start(void);

// True once the camera pipeline is streaming (and not paused).
bool face_detection_running(void);

// True once the detection task has decided the face model will NOT be loaded
// this session — missing/truncated files, or not enough internal RAM (the
// debug WiFi AP takes it permanently, see wifi_ap.h). The preview still runs,
// but nothing will ever detect a face, so anything that gates on a face must
// say so rather than time out on every student. False while it is still
// loading, so a check right after face_detection_start() cannot misfire.
bool face_detection_model_unavailable(void);

// Pauses capture: idles the detection task and turns the sensor stream off,
// keeping the pipeline initialized so face_detection_start() resumes instantly
// (no risky deinit / re-bring-up). Safe to call when not running.
void face_detection_stop(void);

// Copies the newest preview frame (RGB565, FACE_PREVIEW_W*H*2 bytes — `dst`
// must be at least that large) and up to `max_boxes` face boxes into `boxes`.
// Returns the number of boxes copied, or -1 when no frame is available yet.
// Thread-safe; call from the LVGL thread.
int face_detection_snapshot(uint8_t* dst, face_box_t* boxes, int max_boxes);

// Latest human-readable status line ("model ready", "faces 2  infer 40ms", ...).
void face_detection_status(char* out, size_t cap);

// Multi-line model diagnostic: each model's full path, size, and whether the
// detector loaded (or why not). Set once at startup; useful for confirming the
// SD models are found. Thread-safe.
void face_detection_model_info(char* out, size_t cap);

// Requests that the next frame be saved, scaled to SNAPSHOT_W x SNAPSHOT_H
// (via photo_store). Picked up by the detection task; returns immediately.
void face_detection_request_capture(void);
