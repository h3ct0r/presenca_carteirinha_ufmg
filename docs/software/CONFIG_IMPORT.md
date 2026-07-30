# Config Import — the tar contract (device ⇄ config-builder)

This is the **authoritative contract** for the offline config-import feature. It
is referenced by **both** halves of the feature, which are developed separately:

1. **`tools/config-builder/`** — an off-device, browser-only tool that a user
   runs on a laptop to author a config tree and **produce a `.tar`**.
2. **The device import endpoint** (firmware, `src/services/…`, not yet built) —
   receives the `.tar`, stages it, validates it, and applies it to the SD card.

Neither side may drift from this file. **This document is the sync point.** If a
schema or rule changes, change it here first, then update both sides.

> **Schema authority:** the JSON schemas + validation rules below mirror what the
> firmware already enforces at load time in
> [`src/services/roster_service.cpp`](../../src/services/roster_service.cpp) and
> [`src/services/config_service.cpp`](../../src/services/config_service.cpp). Those
> files are the ground truth for the *device's* behavior; this doc restates them
> so the web tool can validate identically **before** producing a tar. When they
> disagree, the firmware wins and this doc is the bug.

---

## 1. What the tar contains

A **plain (uncompressed) POSIX `ustar`** archive whose entries are the
**authored** config files, at paths relative to the SD-card root:

```
config.json
students/students.json
classes/<CODE>/class.json          (one per class, <CODE> matches the "code" field)
students/photos/<id>.jpg           (optional; one per student with an authored avatar)
```

That is the **entire** allowed set. See §4 for why nothing else is permitted.
Student avatars (`students/photos/<id>.jpg`) are optional and additive — a tar
without them is still valid; see [`STUDENT_PHOTOS.md`](STUDENT_PHOTOS.md).

Reference sample tree: [`docs/sd_card_example/`](sd_card_example/). Authoring a
tar is literally "edit that tree, then archive these three kinds of file."

## 2. Merge, don't replace — the safety-critical rule

The device **produces** data that was never authored on a laptop and must
**survive** an import:

| Path | Origin | On import |
|------|--------|-----------|
| `/config.json` | authored | **overwrite** |
| `/students/students.json` | authored | **overwrite** |
| `/students/photos/<id>.jpg` | authored (avatars) | **overwrite** (create dir if new) |
| `/classes/<CODE>/class.json` | authored | **overwrite** (create dir if new) |
| `/classes/<CODE>/attendance/**` | device (roll-call) | **preserve — never touch** |
| `/photos/**` | device (snapshots) | **preserve** |
| `/students/checkins/**` | device (check-in snapshots) | **preserve** |
| `/csv_export/**` | device | **preserve** |
| `/backup/**` | device (pre-wipe snapshot) | **preserve** |
| `/models/**` | provisioning (large) | **preserve** |

An import is an **overlay of the three authored files**, not a filesystem
replace. Replacing the tree would erase attendance — the whole point of the
device. The device importer enforces this via the path whitelist in §4; the
config-builder must **never emit** a tar entry outside the authored set.

> If a class in the tar has a `<CODE>` that no longer matches any existing
> folder, its `class.json` is created fresh (empty attendance). If a class that
> exists on the device is **absent** from the tar, its folder (and attendance)
> is **left as-is** — import never deletes classes. Removing a class is a
> separate, explicit device action, not a side effect of import.

## 3. JSON schemas

Fields and limits below are exact. String limits are the firmware's buffer sizes
(a value at the limit must fit **including** the NUL terminator, so treat the
number as "max characters − 1"). The web tool should reject over-limit input at
authoring time with a clear message, because the device will reject the whole
import otherwise.

### 3.1 `config.json`
```jsonc
{
  "teachers": [                          // 1..8 (CONFIG_MAX_TEACHERS); extras ignored by device
    {
      "name":     "Prof Hector Azpurua", // ≤ 47 chars
      "email":    "hector@dcc.ufmg.br",  // ≤ 63 chars; links classes → teacher
      "rfid_uid": "E0:D1:33:5F",         // ≤ 39 chars; professor card; may be "" (password login)
      "password": "1234"                 // ≤ 31 chars, DIGITS ONLY; may be "" (card login); see rules
    }
  ]
}
```
Rules (mirror `config_service.cpp`):
- At least **one** teacher; **≤ 8** are honored.
- `password` must be **digits only** (`0-9`) — it's typed on a numeric keypad
  (`CONFIG_NON_NUMERIC_PASSWORD`). Empty is allowed (that teacher logs in by card).
- Passwords must be **unique** across teachers (`CONFIG_DUP_PASSWORD`) — a typed
  password must identify exactly one professor. Two empty passwords are fine.

### 3.2 `students/students.json`
```jsonc
{
  "version": 1,
  "students": [                          // 0..600 (ROSTER_MAX_STUDENTS)
    { "id": "2023-0142", "name": "Maria Santos", "rfid_uid": null }
  ]
}
```
Rules (mirror `roster_service.cpp`):
- `id` — the **stable key** (university id / *matricula*), ≤ 19 chars, non-empty,
  **unique** across the file (`duplicate id`). May contain dashes.
- `name` — ≤ 47 chars, non-empty. May contain Portuguese accented characters
  (stored UTF-8). *Device note: the LVGL fonts must include the glyphs to render
  them — see `docs/software/CUSTOM_FONT_GENERATION.md`.*
- `rfid_uid` — `null` (unbound; the common authored state — cards are bound on
  the device at first tap) or a string ≤ 23 chars. If a string, its **normalized**
  form must be unique across students **and** must not equal any teacher's
  `rfid_uid` (a card belongs to one holder). Prefer emitting `null` from the
  builder unless the author explicitly provides a card.
- The student registry carries **no** class-group tag — a student is global and
  may belong to several classes/groups. The group (*turma*) is recorded **per
  class**, on each `class.json` roster entry (see §3.3).
- File ≤ **200 KB** (`STUDENTS_MAX_BYTES`).

> **UID normalization:** the device compares UIDs after `uid_normalize()`
> (uppercase hex, separators stripped — see `src/app/uid.cpp`). The builder's
> uniqueness check must normalize the same way, or two "different-looking" UIDs
> that collide on-device will pass the builder and fail the import.

### 3.3 `classes/<CODE>/class.json`
```jsonc
{
  "version": 1,
  "code": "2026_2-DCC219",               // ≤ 23 chars; MUST equal the folder <CODE>
  "name": "DCC219",                      // ≤ 47 chars
  "schedule": "",                        // ≤ 39 chars
  "teacher_emails": [                    // 1..8 professors; a class may be co-taught
    "hector@dcc.ufmg.br",                // each ≤ 63 chars; each should match a config.json teacher
    "aline@dcc.ufmg.br"
  ],
  "color": "272766",                     // 6-hex RGB, no leading '#'
  "capture_photos": false,               // optional; kiosk photo check-in (default false)
  "face_verify_seconds": 15,             // optional; face-verify countdown, 3..60 (default 15)
  "timed_attendance": false,             // optional; arrival tap + confirm tap (default false)
  "min_attendance_min": 45,              // optional; timed threshold minutes (default 45)
  "roster": [                            // 0..100 (ROSTER_MAX_CLASS_STUDENTS)
    { "id": "2023-0142", "turma": "TE1" }, // turma optional, ≤ 15 chars
    { "id": "2023-0187", "turma": "TE2" }
  ]
}
```
The four attendance fields (`capture_photos`, `face_verify_seconds`,
`timed_attendance`, `min_attendance_min`) are **optional, per-class, and edited
on the device** (in the class ⚙ settings). Photo check-in is a class-only
option — there is **no** device-wide capture flag. The config-builder does not
emit them, so an imported class.json omitting them resets the device to the
defaults above (like any authored field the importer overwrites).
Rules (mirror `roster_service.cpp`):
- **≤ 12 classes** total (`ROSTER_MAX_CLASSES`).
- `teacher_emails` is an **array of 1..8** professor emails
  (`ROSTER_MAX_CLASS_TEACHERS`, which mirrors `CONFIG_MAX_TEACHERS`). A class is
  listed for a professor when **any** entry matches their `config.json` email
  (compared case-insensitively). Extra entries beyond 8 are ignored with a
  warning; blank entries are skipped. **Legacy:** a scalar
  `"teacher_email": "…"` is still accepted when `teacher_emails` is absent and
  becomes a single entry, so cards written before multi-professor support keep
  working. The config-builder always emits the array form.
- `code` **unique** across classes, and the **folder name must equal `code`**
  (the device uses the folder as the class `dir`). When authored from a *Diário*
  import the builder derives it as `<SEMESTER>-<ATIVIDADE>` (the header's
  `PERIODO`+`ATIVIDADE`, e.g. `2026_2-DCC219`). The semester's `/` is not a legal
  folder char, so it is replaced with `_` (`2026/2` → `2026_2`). Every *turma* of
  the same course+semester therefore **merges into one class**; students are
  distinguished by their per-roster-entry `turma` (§3.3), not by separate classes.
- `roster[]` entries are objects `{ "id", "turma"? }` (a bare `"id"` string is
  also accepted). `id` must exist in `students.json` (`roster referencing unknown
  id`) — a **cross-file** check, validated over the whole set. `turma` is the
  **optional per-student class-group** tag (≤ 15 chars, e.g. `"TE1"`; the Diário
  header's `TURMA`). Because it lives on the roster entry, one class may hold
  students from **different turmas**, and the same student can carry a different
  turma in another class. The device reads only `id` from a roster entry and
  **preserves** any `turma` untouched on rewrite (it is not yet used at runtime).
- **A student appears at most once per class** — one turma per student per class.
  The same `id` listed twice in a roster (even with different `turma` values) is
  rejected (`student … listed twice`); the device enforces this at load and the
  builder before export. (A student may of course be in *several* classes, with a
  different turma in each.)
- `class.json` ≤ **32 KB** (`CLASS_MAX_BYTES`).

## 4. Tar format + path security

- **Format:** POSIX `ustar`, 512-byte blocks, **uncompressed**. (Config is small
  JSON; gzip buys nothing and would force a decompressor onto the device. Revisit
  only if photos are ever bundled — currently they are not.)
- **Entry names:** relative, forward slashes, **no leading `/`**, **no `..`**, no
  drive/absolute prefixes. Directory entries are optional (the device may `mkdir`
  on demand); if emitted, they end in `/`.
- **Whitelist (device enforces, builder respects):** an entry is accepted **only**
  if it matches one of:
  - `config.json`
  - `students/students.json`
  - `classes/<seg>/class.json` where `<seg>` has no `/` or `..`
  - `students/photos/<seg>.jpg` where `<seg>` has no `/` or `..` (a student id;
    the builder re-keys avatars name→id — unknown ids are harmless on the device,
    they simply never display)
  Anything else — especially any `attendance/` path, `..`, or an absolute path —
  is **rejected and aborts the whole import** (classic tar/zip-slip defense; this
  is the S1 concern, so it is not optional).
- **Size:** the device caps the staged tar at **≤ 16 MB** (raised from 1 MB to
  fit bundled avatars — ~600 students of 100×100 JPEGs is < 6 MB; see
  STUDENT_PHOTOS.md §9). Oversize → reject. The current importer reads the tar
  into a single PSRAM-backed buffer; a true streaming unpack (STUDENT_PHOTOS.md
  §3) is a future optimization, not required at this cap.

## 5. Device import flow (firmware side — IMPLEMENTED)

Implemented in [`src/services/import_service.cpp`](../../src/services/import_service.cpp)
(`import_service_run`). Guarantees, in order:

1. **Read** the staged tar (`/config.tar`) into a size-capped heap buffer
   (≤ 16 MB, from PSRAM — never the 128 KB LVGL heap).
2. **Structural + §4 check:** parse the ustar and enforce the name whitelist on
   every entry ([`src/app/ustar.cpp`](../../src/app/ustar.cpp)). A disallowed
   path (`..`, absolute, non-whitelisted) aborts here — before anything is
   touched.
3. **Back up first:** `backup_store_create()`
   ([`src/storage/backup_store.cpp`](../../src/storage/backup_store.cpp))
   snapshots the current authored files into `/backup/previous/`. If the backup
   fails, the import aborts — never modify live config without a safety net.
4. **Unpack** into `/import_staging/` (cleaned first, so a stale class from a
   prior run can't leak in), re-applying the §4 whitelist per entry.
5. **Validate** the staged tree with the exact live rules, WITHOUT mutating live
   state — `config_validate_tree()` / `roster_validate_tree()`. **Any failure →
   discard staging, live config untouched.**
6. **Apply** each authored file into place with the atomic `temp → remove →
   rename` pattern, per file — the three JSON kinds **and** any staged
   `students/photos/<id>.jpg` avatars (dir created if new, files overwritten).
   Attendance / device `/photos/**` / `/students/checkins/**` / models / backup
   are never named here, so they are preserved in place.
7. **Reload** config + roster services (`*_service_reload()`), which republish
   status so the UI refreshes on its own.
8. **Sentinel:** on the SD path, rename `/config.tar → /config.tar.imported` so
   it is not re-imported on the next boot.

Live config is only touched at step 6, so a power loss earlier leaves a working
device. **Revert:** `import_service_revert()` re-applies `/backup/previous/` as
a tree (validate + apply + reload, no new backup). Because import is an overlay
(§2), a revert restores the authored files but does **not** remove a class the
import *added* — removing a class stays a separate, explicit action.

## 6. Upload interface

**v1 (implemented):** the config-builder only **generates and downloads** the
`.tar`. To install it, the operator gets `config.tar` onto the **SD-card root** —
either by copying it there on a laptop, or by uploading it to root through the
device's Wi-Fi file manager
([`src/services/file_server.cpp`](../../src/services/file_server.cpp)). Either way
it lands at `/config.tar`, which the device detects (`import_service_pending()`)
and offers to apply **confirm-first**:
- the **idle screen** shows "Import config from SD" whenever a valid config is
  missing — the new-device path, reachable with no login;
- the **Admin panel** shows "Import configuration" for a logged-in professor
  (the field-update path), plus "Restore previous configuration" when a backup
  exists.

Uploading to the SD root reuses the exact same detection as inserting a card that
already has the tar, so there is one code path, not two. The builder needs **no**
network code and stays a pure static page.

**v2 (optional later):** a dedicated `POST /api/import` on the device
(`multipart/form-data`, field `archive`), gated by a professor password field, so
the builder can upload directly. Adds network + auth to the builder; only do it
if the manual step proves annoying.

Either way the write path is **auth-gated + AP-only** (the AP already has a
per-boot WPA2 password). An unauthenticated "replace my config" endpoint is
worse than the current file editor — do not ship one.

## 7. Versioning

- `students.json` and `class.json` carry `"version": 1`. If a schema changes
  incompatibly, bump the version, teach the device to read old+new, and record
  the change in this file's changelog below.
- The tar itself is unversioned; the file schemas version themselves.

### Changelog
- **2026-07-29** — **a broken class folder no longer blanks the whole class
  list.** Because an import is an **overlay** (§2), importing a config whose
  classes have different codes leaves the previous `/classes/<OLD>/` folders on
  the card. Their rosters reference students the new `students.json` no longer
  has, and the loader used to treat that as fatal — so the class screen showed
  *"No class data"* and even the freshly imported classes were hidden. The **live**
  load now **skips** an unloadable class (counted + explained via
  `roster_skipped_class_count()` / `roster_get_skip_reason()`, surfaced as a
  notice on the class list) and keeps the rest. **Import-time validation is
  unchanged and still strict** — a tar containing a bad class is rejected before
  it is applied. Stale folders are still not deleted, so attendance is never lost;
  remove them by hand (or via the WiFi file manager) if you want them gone.
- **2026-07-29** — **a class can be co-taught by several professors.** `class.json`
  §3.3 `teacher_email` (scalar) → **`teacher_emails`** (array of 1..8, capped by
  `ROSTER_MAX_CLASS_TEACHERS` = `CONFIG_MAX_TEACHERS`). A class is listed for a
  professor when **any** entry matches. **Backward compatible:** the device still
  reads a legacy scalar `teacher_email` when the array is absent, so existing
  cards keep working; the config-builder now always emits the array and offers a
  professor checkbox list per class. Firmware: `class_rec_t.teacher_emails[][64]`
  + `teacher_count`, `roster_class_matches_teacher()` matches any.
- **2026-07-28** — **student avatars are now bundled in the tar.** Added a fourth
  authored kind, `students/photos/<id>.jpg` (§1, §2, §4), overwritten on import
  beside `students.json` (dir created if new); device `/photos/**` and
  `/students/checkins/**` are preserved. Raised the staged-tar cap **1 MB → 16 MB**
  (§4, §5) to fit bundled avatars. Enforced by `ustar_name_allowed`
  (`src/app/ustar.cpp`) and applied by `import_service.cpp`; the config-builder
  re-keys Moodle photos name→id and packs them (STUDENT_PHOTOS.md).
- **2026-07-28** — **photo check-in is now class-only.** Removed
  `capture_photos` from `config.json` (§3.1) — there is no device-wide capture
  flag. Documented the optional per-class attendance fields in `class.json`
  (§3.3): `capture_photos`, `face_verify_seconds`, `timed_attendance`,
  `min_attendance_min` (device-edited; the builder omits them; defaults apply on
  import). Config-builder dropped its "Device options / capture" section.
- **2026-07-27** — **device import side implemented** (§5, §6 v1). Added the
  on-device ustar reader (`app/ustar`), the pre-apply snapshot
  (`storage/backup_store`, `/backup/previous/`), and the pipeline
  (`services/import_service`: structural+§4 check → backup → staged unpack →
  non-mutating validate → atomic apply → reload → sentinel), with `revert`.
  Config/roster loaders gained non-mutating `*_validate_tree` seams. Trigger is
  a `config.tar` at the SD root (inserted or uploaded to root), applied
  confirm-first from the idle screen (new device) or the Admin panel (field
  update). No tar-format or schema change. No firmware `POST /api/import` (v2
  still deferred).
- **2026-07-25** — added an optional **per-student `turma`** tag (≤ 15 chars) on
  each **`class.json` roster entry** (`{ "id", "turma" }`), populated by the
  config-builder's *Diário de Classe* CSV import. It lives on the roster entry —
  **not** on the global student registry and **not** as a class-level field — so
  one class can span turmas and a student can differ per class. **Additive and
  backward-compatible:** the firmware reads only `id` from a roster entry and
  preserves `turma` on rewrite (DOM edit in `persist_enroll`), so `version` stays
  **1** and no `student_t`/`class_rec_t` change was needed.
- **2026-07-25** — initial contract (v1 schemas), extracted from the shipping
  firmware validators.
