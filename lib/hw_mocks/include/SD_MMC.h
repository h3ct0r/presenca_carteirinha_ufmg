#pragma once

// Mock of the SD_MMC singleton over the in-memory store (see mock_sd.h for
// the test-control API).

#include "FS.h"

class MockSDMMC {
 public:
    bool begin(const char* mountpoint = "/sdcard", bool mode1bit = false);
    bool exists(const char* path);
    File open(const char* path, const char* mode = FILE_READ, bool create = false);
    bool mkdir(const char* path);
    bool rmdir(const char* path);  // empty directories only, like the real one
    bool remove(const char* path);
    bool rename(const char* from, const char* to);
    uint64_t cardSize();
    uint64_t totalBytes();
    uint64_t usedBytes();
};

extern MockSDMMC SD_MMC;
