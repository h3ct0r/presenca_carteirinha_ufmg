#pragma once

#include "ui/screen.h"

// Camera preview screen: shows the live downscaled feed with face-detection
// boxes (when detection is built in) and a snapshot button. Drives the
// face_detection_service via on_show/on_hide.
extern const screen_t scr_camera;
