#pragma once

#include <stddef.h>

// Offline config import: applies a config.tar (produced by the config-builder)
// to the SD card. See docs/software/CONFIG_IMPORT_PLAN.md (step 4) and
// docs/software/CONFIG_IMPORT.md §4-§5 for the tar contract and flow.
//
// The pipeline is: read the staged tar -> structural + §4 whitelist check ->
// back up the current authored config (backup_store) -> unpack to a staging
// tree -> validate the staging tree with the live rules (config/roster
// *_validate_tree) -> atomically apply each authored file into place
// (attendance/photos/models/backup preserved) -> reload the config + roster
// services. The live config is only touched at the apply step, so any earlier
// failure leaves a working device untouched.

typedef struct {
    bool ok;
    char message[128];  // human-readable result, for the on-screen toast
} import_result_t;

// The path the device treats as a pending import: the SD-card root. A card
// inserted with this file, or the same file uploaded to root through the web
// file manager, both land here.
const char* import_service_tar_path(void);  // "/config.tar"

// True if a pending import archive is present at import_service_tar_path().
bool import_service_pending(void);

// Runs the full pipeline over the tar at `tar_path`. On success against the SD
// tar path, the source is renamed to a sentinel so it is not re-imported every
// boot. On any failure before the apply step, the live config is untouched.
// Call on the LVGL/import thread (SD I/O; reloads the services).
import_result_t import_service_run(const char* tar_path);

// Restores the last pre-import snapshot (backup_store) by re-applying it as a
// tree. Does NOT create a new backup (that would clobber the snapshot being
// restored). Returns {ok,message}; fails if no snapshot exists. Note: like any
// import this is an overlay — a class the import *added* is not removed by a
// revert (see CONFIG_IMPORT_PLAN.md). LVGL/import thread.
import_result_t import_service_revert(void);
