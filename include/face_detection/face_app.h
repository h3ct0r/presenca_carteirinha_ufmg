#ifndef _FACE_APP_H
#define _FACE_APP_H

// Builds the tabview UI (Face Detect tab + Widgets tab) and starts the
// camera + detection task. Call once from setup() after lv_init and the
// display/touch are registered. Camera hardware is optional: without it the
// tab shows the error state.
void face_app_start();

#endif
