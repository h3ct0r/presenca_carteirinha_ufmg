#pragma once

// Full-screen face-verification overlay used to gate a check-in when photo
// capture is enabled (see docs/software/FACE_CHECKIN.md). Shows the live camera
// preview with the detected face box and a countdown; the student must show a
// face within the window.
//
// On a face detected in time it saves a check-in photo under
// /students/checkins/<id>/<date>_<code>_<NN>.jpg and calls done(true); on
// timeout it calls done(false). The overlay closes before done() is invoked.
//
// Requires the camera warmed (face_detection_start) — the caller checks
// face_detection_running() first and shows its own alert otherwise. One verify
// at a time. LVGL thread only.

typedef void (*face_verify_done_cb)(bool verified);

void face_verify_open(const char* student_id, const char* student_name, const char* date,
                      const char* class_code, int timeout_s, face_verify_done_cb done);

// Closes any open verify overlay without calling the callback (e.g. on screen
// teardown). Safe when nothing is open.
void face_verify_cancel(void);
