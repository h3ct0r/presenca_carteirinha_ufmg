#pragma once

#include <stddef.h>

// Test-control surface of the in-memory SD card. The production code sees
// the same API as on the device (FS.h / SD_MMC.h in this mock library);
// tests use these helpers to seed card contents and inspect written files.
void mocksd_reset(void);                    // empty card, begin() succeeds
void mocksd_set_begin_result(bool ok);      // simulate missing/unreadable card
void mocksd_add_file(const char* path, const char* contents);  // + parent dirs
void mocksd_add_dir(const char* path);
bool mocksd_exists(const char* path);
size_t mocksd_file_size(const char* path);
// Copies up to cap bytes of the file into out; returns bytes copied.
size_t mocksd_read_file(const char* path, void* out, size_t cap);
