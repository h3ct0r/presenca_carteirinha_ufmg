#pragma once

#include <stddef.h>
#include <stdint.h>

// Mounts the TF card (SDMMC slot 0, powered by the on-chip LDO) and starts
// the background writer task. Returns false if no card is present; the rest
// of the API stays safe to call either way.
bool photo_store_init();

// Snapshots an RGB565 frame to /photos/IMG_nnnn.bmp on the card. Copies the
// frame and returns immediately; the BMP conversion and SD write happen on
// the writer task. Returns false if the card is missing, a save is already
// in flight, or the copy buffer can't be allocated. Safe to call from any
// task (the caller must guarantee the frame stays valid during the call).
bool photo_store_capture(const uint8_t* rgb565, int w, int h);

// Copies the current human-readable state ("SD ready", "saving...",
// "saved IMG_0007.bmp", ...) into out. Thread-safe.
void photo_store_get_status(char* out, size_t out_len);
