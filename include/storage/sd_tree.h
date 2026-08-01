#pragma once

#include <stddef.h>

#include "app/progress.h"

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
//
// No progress callback on purpose: the only caller is the web file manager's
// delete handler, which runs on the `fileserv` task with no UI to report to.
bool sd_tree_remove(const char* path, sd_tree_stats_t* out, char* err, size_t err_cap);

// Deletes every entry at the card root except one: `keep` is a bare filename
// (e.g. "config.json"), compared case-insensitively because the card is FAT.
// Directories go recursively, exactly like sd_tree_remove().
//
// Returns true only when every targeted entry is gone. `out` (optional) counts
// what went and what resisted; `err` (optional) gets a short reason. A `keep`
// that isn't at the root is not an error — there is simply nothing to spare.
//
// This is the debug "erase the card" tool. Everything the device produced
// (rosters, attendance, photos, exports, face models, backups) is destroyed;
// only the named file survives.
//
// `cb` (optional) is called after each entry is removed, so the UI can show that
// a long wipe is progressing. The count is only discovered while recursing, so
// it reports total = 0 and a rising `done`. This one matters: a silent wipe of a
// full card looks hung, and an operator who power-cycles it mid-way is left with
// a half-erased card.
bool sd_tree_wipe_root(const char* keep, sd_tree_stats_t* out, char* err, size_t err_cap,
                       progress_cb_t cb, void* ctx);

// Renames the file or directory at `path` to `new_name` **within its own
// parent directory** — `new_name` is a bare name, not a path. Returns false
// with a reason in `err` for an invalid name, a missing source, or a name that
// is already taken.
//
// Note the card is FAT: names are case-insensitive, so a case-only rename
// ("notes.txt" -> "Notes.txt") is rejected as already existing.
bool sd_tree_rename(const char* path, const char* new_name, char* err, size_t err_cap);
