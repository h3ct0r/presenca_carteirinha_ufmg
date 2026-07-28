#pragma once

#include <stddef.h>
#include <stdint.h>

// Mounts the TF card (SDMMC slot 0, powered by the on-chip LDO) and starts
// the background writer task. Returns false if no card is present; the rest
// of the API stays safe to call either way.
bool photo_store_init();

// Snapshots an RGB565 frame to /photos/IMG_nnnn.jpg on the card. Copies the
// frame and returns immediately; the encode + SD write happen on the writer
// task. Saved as JPEG via the P4 hardware encoder (quality 85, 4:2:0 — ~300-500
// KB at 1080p); only if the JPEG engine is unavailable does it fall back to an
// uncompressed .bmp. Returns false if the card is missing, a save is already
// in flight, or the copy buffer can't be allocated. Safe to call from any
// task (the caller must guarantee the frame stays valid during the call).
bool photo_store_capture(const uint8_t* rgb565, int w, int h);

// Synchronously hardware-encodes an RGB565 frame to a JPEG at `path` (the file
// is created/overwritten). For per-student check-in photos, whose frame the
// caller already holds (the camera preview buffer). Returns false if the encoder
// is unavailable, a /photos capture is in flight, or any step fails. Reuses the
// single hardware encoder — do not call it concurrently with photo_store_capture.
bool photo_store_encode_to(const char* path, const uint8_t* rgb565, int w, int h);

// Copies the current human-readable state ("SD ready", "saving...",
// "saved IMG_0007.bmp", ...) into out. Thread-safe.
void photo_store_get_status(char* out, size_t out_len);

// Full path of the most recently saved photo (e.g. "/photos/IMG_0007.jpg"),
// plus a counter that increments on each successful save. A poller can compare
// the returned counter against the last one it saw to detect a fresh photo and
// show its path. Copies the path into out (empty until the first save); out may
// be NULL to read the counter only. Thread-safe.
uint32_t photo_store_last_saved(char* out, size_t out_len);
