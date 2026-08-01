#pragma once

#include <stddef.h>
#include <stdint.h>

// Crash-safe replacement of a whole file on FAT.
//
// The obvious sequence — write <path>.tmp, remove <path>, rename tmp over it —
// has a window in which NEITHER file exists. The two files this device replaces
// that way are /students/students.json (the entire roster) and /config.json (the
// only way to unlock the device), so losing one to a power cut is not a
// theoretical concern on a battery-powered classroom device with a power button.
//
// The sequence here keeps a recoverable copy at every instant:
//
//   1. write   <path>.tmp          (old file untouched)
//   2. rename  <path> -> <path>.bak
//   3. rename  <path>.tmp -> <path>
//   4. remove  <path>.bak
//
// Cut power at any point and one of these holds the complete old-or-new content:
//   between 1-2  -> <path> is the old file
//   between 2-3  -> <path> is gone, <path>.bak is the old file
//   between 3-4  -> <path> is the new file, <path>.bak is a stale leftover
//
// Only the 2-3 window needs help on the next boot, which is what
// atomic_file_recover() does. Every reader of a file written through
// atomic_file_write() MUST call it first.

// Writes `len` bytes to `path` by the sequence above. Returns false (leaving the
// previous contents reachable) on any failure. `what` names the file in log
// messages, e.g. "students.json".
bool atomic_file_write(const char* path, const uint8_t* data, size_t len, const char* what);

// Restores `path` from `path`.bak when a previous write was interrupted between
// steps 2 and 3, and clears a stale `.bak` left by an interruption between 3 and
// 4. Cheap and idempotent: call it before reading. Returns true if it recovered
// the file (worth logging as a real event — it means a write was interrupted).
bool atomic_file_recover(const char* path);
