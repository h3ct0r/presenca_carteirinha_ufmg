#pragma once

#include <stddef.h>

// Whole-tree SD operations for the debug file manager (see file_server.h):
// recursive delete and rename. Kept out of file_server.cpp so the path handling
// and the recursion are testable natively.
//
// SD I/O, no locking — call from the LVGL thread only, like every other
// storage/ module.

typedef struct {
    int removed;  // files + directories actually deleted
    int failed;   // entries that could not be deleted
} sd_tree_stats_t;

// True when `path` exists and is a directory.
bool sd_tree_is_dir(const char* path);

// Deletes `path`; when it is a directory, everything under it goes too.
// Returns true only if the whole tree is gone. `out` (optional) receives the
// counts, `err` (optional) a short reason on failure.
//
// Refuses the card root. Directories nested deeper than 8 levels are reported
// as failed rather than recursed into, so a pathological tree can't run the
// LVGL thread's stack out.
bool sd_tree_remove(const char* path, sd_tree_stats_t* out, char* err, size_t err_cap);

// Renames the file or directory at `path` to `new_name` **within its own
// parent directory** — `new_name` is a bare name, not a path. Returns false
// with a reason in `err` for an invalid name, a missing source, or a name that
// is already taken.
//
// Note the card is FAT: names are case-insensitive, so a case-only rename
// ("notes.txt" -> "Notes.txt") is rejected as already existing.
bool sd_tree_rename(const char* path, const char* new_name, char* err, size_t err_cap);
