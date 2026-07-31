#pragma once

#include <stdint.h>

// Debug drain log: one CSV row per battery sample, for working out how long a
// charge actually lasts. Off unless the firmware is built with
// -D BATTERY_DRAIN_LOG (see platformio.ini) — the call site in main.cpp is what
// the flag removes, not this module.
//
//   /battery.csv
//   uptime_s,mv,pct
//   10,4148,98
//   20,4147,98
//
// **uptime_s is time since boot, not wall-clock time** — the board has no RTC.
// It restarts at 0 on every reboot, so a log spanning a restart is two runs
// concatenated, not one continuous curve. Sort/plot per run.
//
// Append-only and closed after every row, so an unplanned power-off loses at
// most the last sample. SD I/O with no locking: call it from the LVGL thread
// (main.cpp's app_dispatch), never from the battery task itself.

// Appends one sample, writing the CSV header first if the file is new.
// Returns false on a write error.
bool battery_log_append(uint32_t uptime_s, uint16_t mv, uint8_t pct);

// Where the log lives ("/battery.csv"), for logs and the file manager.
const char* battery_log_path(void);
