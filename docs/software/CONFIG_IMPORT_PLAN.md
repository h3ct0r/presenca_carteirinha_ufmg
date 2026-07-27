# Config Import — device-side implementation plan

Companion to [`CONFIG_IMPORT.md`](CONFIG_IMPORT.md) (the authoritative tar
contract). That file is the *what* (schemas, the merge-don't-replace table, the
§4 path whitelist); **this file is the *how*** for the firmware side — the
device import endpoint that the contract calls "not yet built."

Status: **planned, not yet implemented.** The `tools/config-builder/` already
produces a valid `config.tar`; nothing on the device unpacks one yet.

## Decisions locked in

- **Setup story:** a new device gets its SD prepared on a laptop anyway (format
  FAT32 + copy the large `/models/*.espdl`), so dropping `config.tar` in that
  same step is the lowest-friction path. The tar only carries the **three
  authored files**; media (models/photos) and device-produced data
  (attendance/exports/backups) are out of the tar by design — see
  `CONFIG_IMPORT.md` §2.
- **One importer, two triggers.** A single `import_service` runs the §5 pipeline
  over *a path to a staged tar*. Trigger A = SD-root auto-import (offline, the
  new-device path). Trigger B = web upload via the existing file manager (the
  field-update path). The logic is **not forked** — only the source differs.
- **Confirm-first.** When a *valid* config already exists and a `config.tar` is
  present, import requires an on-screen confirmation ("Import config.tar? Your
  current config is backed up first."). The backup makes it reversible either
  way.
- **Deferred:** a dedicated `POST /api/import` (contract §6 v2); numbered backup
  slots (single-slot for now); any student-vs-teacher UID cross-check at import
  (it is **not** a device *load* rule, only an *enroll* rule — the importer
  mirrors load exactly).

---

## Design piece 1 — the validator refactor

**Problem.** Validation is currently *fused* with reading fixed paths and
writing live static state. `load_config()` reads `/config.json` → validates →
writes `s_config`. `load_students()` / `load_one_class()` parse-and-write
straight into `s_students` / `s_classes`, and the cross-file check ("a roster
references a known student id") only works because students are loaded into
those static arrays first. There is no "validate this tree without applying it"
seam — which is exactly what confirm-first + validate-before-apply
(`CONFIG_IMPORT.md` §5 step 4) needs against `/import_staging/`.

**Refactor.** Thread a `root` prefix through the loaders and expose a
non-mutating validate entry point per service. The two services need *different*
mechanics, driven purely by memory:

- **`config_service` (small — `device_config_t` ≈ 1.5 KB): "fill a caller
  struct."** Extract the parse+validate body of `load_config()` into
  `parse_config_file(const char* path, device_config_t* out, char* err, size_t cap)`
  that writes to a **caller-provided** struct and touches no static state.
  - Live load = call it with a local, then copy into `s_config` under the lock +
    publish (behavior unchanged).
  - `config_validate_tree("/import_staging", msg, cap)` = call it with a
    throwaway local; return ok/msg. Zero live mutation.
  - Keep the existing heap-buffer discipline (read the file into a `malloc`'d
    2 KB buffer, not a stack array) so validation deep in the import call chain
    doesn't stack 2 KB + a `JsonDocument` and overflow the task stack — the code
    already does this for `config_set_password`.

- **`roster_service` (large — `s_students[600]` + `s_classes[12]` ≈ 80 KB of
  static arrays that cannot be duplicated on a stack): "load-from-root then
  restore, under one lock hold."** Parameterize
  `load_students`/`load_classes`/`load_one_class` to build paths from a `root`
  prefix (`""` for live → `/students/...`; `"/import_staging"` for staging).
  Then `roster_validate_tree("/import_staging", msg, cap)`:
  1. take `s_lock`,
  2. `load_all_from("/import_staging")` → capture status + error string,
  3. `load_all_from("")` to **restore** the live arrays from disk,
  4. release `s_lock`, return the captured result.

  Because the whole dance is under the single lock the enroll writes and the
  retry task also use, **no other task ever observes the staging data** — the
  shared arrays are scratch and are put back before the lock is released. Reuses
  100% of the loader, no second 80 KB buffer.

**Deliberate non-goal.** The importer validates *exactly what the device
enforces at load* — nothing more. The device does **not** cross-check student
UID vs. teacher UID at load (only enroll does), so the importer won't either.
This keeps strict parity with the "mirror the firmware validators" rule in the
contract.

---

## Design piece 2 — the backup-store shape

**Purpose.** Snapshot the *outgoing* authored config before apply so a
confirm-first import is reversible. It sits in the `CONFIG_IMPORT.md` §2 preserve
list (`/backup/**` is never touched by import).

- **What it copies:** only the three authored kinds — `/config.json`,
  `/students/students.json`, `/classes/<CODE>/class.json` for every class dir
  found by **scanning `/classes/` directly** (not via loaded roster state — on a
  broken/new device the roster isn't loaded, which is exactly when we import).
  Never attendance/photos/models (preserved in place anyway; copying them would
  be huge).
- **Format: a file-tree copy, not a tar.** The device only has a ustar *reader*,
  no writer — and a mirrored tree gives a clean reuse: **revert = run the
  importer with `/backup/previous/` as its source**, since the backup is laid out
  identically to a staged tree. No separate restore code path.
- **Location & naming (no RTC):** single rotating slot **`/backup/previous/`**,
  overwritten each import → one level of undo, enough for "operator dislikes the
  result." Numbered slots (`/backup/0001/…`) are a later bump if history is
  wanted.
- **Atomicity:** per-file `temp → remove → rename` copies (the project's existing
  pattern). If backup fails, **abort the import before apply** — never modify
  live config without a safety net. A power cut mid-backup is harmless: live
  files aren't touched until the later apply step, and a partial
  `/backup/previous/` is overwritten next run.
- **Layer:** `storage/backup_store.cpp` (passive SD helper, no task, no hardware
  ownership — alongside `attendance_store`/`photo_store`), so it lands in the
  native `build_src_filter` and is mock-SD testable.

**API:**
```c
typedef struct { bool ok; char message[96]; } backup_result_t;
backup_result_t backup_store_create(void);   // snapshot -> /backup/previous, overwrite; skip missing
bool            backup_store_exists(void);    // a restorable snapshot is present
const char*     backup_store_root(void);      // "/backup/previous" — feed to the importer to revert
```

---

## Implementation steps

Each step ends green on **both** `pio run -e esp32p4` and `pio test -e native`;
every new hardware-free `.cpp` is added to the native `build_src_filter` in
`platformio.ini`. Steps 1–5 are almost entirely native-testable; only step 6 is
device-only.

### Step 1 — `app/ustar` (pure ustar reader)
- New `include/app/ustar.h` + `src/app/ustar.cpp`: iterate a 512-byte-block
  ustar buffer/stream, yield `{name, size, data}`. No SD, no LVGL.
- Enforce §4 at parse time: reject leading `/`, `..`, backslash/drive prefixes,
  and any name outside the whitelist (`config.json`, `students/students.json`,
  `classes/<seg>/class.json`).
- Native tests: the 3 valid entries; zip-slip (`../`, absolute) aborts;
  truncated/short archive; oversize (>1 MB) rejected; a non-whitelisted entry
  aborts the whole archive.

### Step 2 — validator refactor (design piece 1)
- `config_service`: extract `parse_config_file(...)`; `load_config()` becomes a
  thin wrapper; add `config_validate_tree(root, msg, cap)`.
- `roster_service`: thread a `root` prefix through the loaders; add
  `roster_validate_tree(root, msg, cap)` (load-staging → restore-live under one
  lock hold).
- Native tests: `*_validate_tree` returns OK for a good staged tree and the
  correct error per rule; **live state is provably unchanged after a failed
  validation** (assert `config_get`/roster accessors match pre-call state).

### Step 3 — `storage/backup_store` (design piece 2)
- `include/storage/backup_store.h` + `src/storage/backup_store.cpp`:
  `backup_store_create()` (scan `/classes/` directly, per-file temp→rename copy
  into `/backup/previous/`, skip missing), `backup_store_exists()`,
  `backup_store_root()`.
- Native tests: snapshots all three authored kinds; skips absent files on a
  fresh device; leaves attendance/photos/models untouched; overwrites a prior
  snapshot cleanly.

### Step 4 — `services/import_service` (the §5 core) + safety tests
- `import_result_t import_service_run(const char* tar_path)`: backup → unpack to
  `/import_staging/` (whitelist per entry) → `config_validate_tree` +
  `roster_validate_tree` on staging → **abort untouched on any failure** →
  atomic per-file apply (attendance/photos/models/backup preserved) → reload
  config + roster → result string. Plus `import_service_sd_pending()` and a
  post-success rename `/config.tar → /config.tar.imported`.
- **Safety-critical native tests live here:** whitelist aborts the whole import;
  a validation failure leaves live config **and** attendance byte-identical;
  existing attendance/photos survive a successful import; the sentinel rename
  prevents a re-import loop; **revert works by running the importer against
  `backup_store_root()`**.

### Step 5 — web drop path (trigger B, no builder changes)
- Convention: operator uploads `config.tar` to `/import/` via the existing file
  manager (`file_server.cpp` already has `/api/upload?dir=`). No `/api/import`
  (v2 stays deferred).

### Step 6 — UI triggers + confirm-first modal (device build-verified only)
- Shared confirm modal → `import_service_run`.
- **Idle screen:** when config is missing/invalid *and*
  `import_service_sd_pending()`, show **"Import config from SD"** (the new-device
  path).
- **Admin screen:** an **"Import configuration"** action (field-update path from
  the web drop or SD), reusing the modal.
- Honesty rule: report step 6 as build-verified, **not run on hardware** — the
  LVGL modal flow and SD I/O timing only surface on device.

### Step 7 — docs
- Update `CONFIG_IMPORT.md` §5/§6 to record "both triggers, confirm-first," and
  add a **new-device runbook**: format FAT32 → copy `/models/` → drop
  `config.tar` → boot → tap Import. Add a changelog entry.

---

## New-device runbook (target end state)

1. On a laptop: format the SD card FAT32.
2. Copy `/models/*.espdl` onto the card (only if the camera/face-detection
   feature is used).
3. Drop the `config.tar` produced by `tools/config-builder/` at the SD root.
4. Insert the card and boot. The idle screen detects the tar and offers
   **"Import config from SD."**
5. Tap it, confirm. The device backs up any prior config, validates, applies,
   and reloads — then runs normally.
