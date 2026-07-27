#pragma once

// A one-slot snapshot of the device's *authored* config surface, taken right
// before a config-import overwrites it so the operator can revert. See
// docs/software/CONFIG_IMPORT_PLAN.md (design piece 2).
//
// It copies ONLY the three authored file kinds — /config.json,
// /students/students.json, and every /classes/<CODE>/class.json — into
// /backup/previous/, mirroring the SD layout. It never touches the
// device-produced data (attendance, photos, exports, models); those are
// preserved in place by the importer and are far too large to snapshot.
//
// The snapshot lays out identically to a staged import tree, so a revert is
// simply "run the importer with backup_store_root() as its source" — no
// separate restore path.

typedef struct {
    bool ok;
    char message[96];  // human-readable result, for the on-screen toast
} backup_result_t;

// Snapshots the current authored config surface into /backup/previous/,
// overwriting any prior snapshot. Missing source files are skipped (a fresh
// device may have none). Enumerates classes by scanning /classes directly (not
// via the loaded roster, which may be absent exactly when an import runs).
// SD I/O; call on the import (LVGL) thread.
backup_result_t backup_store_create(void);

// True if a restorable snapshot is present (a backed-up /config.json exists).
bool backup_store_exists(void);

// Root of the snapshot tree ("/backup/previous"), to feed to the importer as a
// revert source.
const char* backup_store_root(void);
