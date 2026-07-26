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
> [`src/services/roster_service.cpp`](../src/services/roster_service.cpp) and
> [`src/services/config_service.cpp`](../src/services/config_service.cpp). Those
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
```

That is the **entire** allowed set. See §4 for why nothing else is permitted.

Reference sample tree: [`docs/sd_card_example/`](sd_card_example/). Authoring a
tar is literally "edit that tree, then archive these three kinds of file."

## 2. Merge, don't replace — the safety-critical rule

The device **produces** data that was never authored on a laptop and must
**survive** an import:

| Path | Origin | On import |
|------|--------|-----------|
| `/config.json` | authored | **overwrite** |
| `/students/students.json` | authored | **overwrite** |
| `/classes/<CODE>/class.json` | authored | **overwrite** (create dir if new) |
| `/classes/<CODE>/attendance/**` | device (roll-call) | **preserve — never touch** |
| `/photos/**` | device (snapshots) | **preserve** |
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
  "capture_photos": false,               // bool; take a photo on check-in
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
  "students": [                          // 0..300 (ROSTER_MAX_STUDENTS)
    { "id": "2023-0142", "name": "Maria Santos", "rfid_uid": null }
  ]
}
```
Rules (mirror `roster_service.cpp`):
- `id` — the **stable key** (university id / *matricula*), ≤ 19 chars, non-empty,
  **unique** across the file (`duplicate id`). May contain dashes.
- `name` — ≤ 47 chars, non-empty. May contain Portuguese accented characters
  (stored UTF-8). *Device note: the LVGL fonts must include the glyphs to render
  them — see `docs/CUSTOM_FONT_GENERATION.md`.*
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
  "teacher_email": "hector@dcc.ufmg.br", // ≤ 63 chars; should match a config.json teacher
  "color": "272766",                     // 6-hex RGB, no leading '#'
  "roster": [                            // 0..100 (ROSTER_MAX_CLASS_STUDENTS)
    { "id": "2023-0142", "turma": "TE1" }, // turma optional, ≤ 15 chars
    { "id": "2023-0187", "turma": "TE2" }
  ]
}
```
Rules (mirror `roster_service.cpp`):
- **≤ 12 classes** total (`ROSTER_MAX_CLASSES`).
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
  Anything else — especially any `attendance/` path, `..`, or an absolute path —
  is **rejected and aborts the whole import** (classic tar/zip-slip defense; this
  is the S1 concern, so it is not optional).
- **Size:** the device caps the staged tar (suggested **≤ 1 MB**; config for 300
  students + 12 classes is well under 100 KB). Oversize → reject.

## 5. Device import flow (informative — firmware side)

Documented here so the builder author knows the guarantees. Implemented later in
firmware; not this doc's deliverable.

1. **Receive** the tar over the AP (see §6), streaming to a staging file on SD
   (never buffer in the 128 KB LVGL heap).
2. **Back up first:** call `backup_store_create()` (the S2 pre-wipe snapshot) so
   the prior config is always recoverable if the operator dislikes the result.
3. **Unpack** into a sandbox dir (e.g. `/import_staging/`), applying the §4
   whitelist to every entry name.
4. **Validate** the staged JSON with the same rules as §3 (reuse the
   roster/config validators). **Any failure → discard staging, live config
   untouched.**
5. **Apply** by moving each authored file into place with the project's atomic
   `temp → remove → rename` pattern (per file — FAT has no atomic dir swap, and
   attendance dirs must be preserved in place).
6. **Reload** config + roster services and report a result string to the UI.

Because live config is only touched in step 5, a power loss mid-upload (no
battery/RTC) leaves a garbage staging file and a working device.

## 6. Upload interface

**v1 (recommended, zero device coupling):** the config-builder only **generates
and downloads** the `.tar`. The operator connects to the device AP and uploads it
through the **existing** web file manager
([`src/services/file_server.cpp`](../src/services/file_server.cpp)) into a known
drop path; a device button (or a watched path) triggers the import flow. The
builder needs **no network code** and stays a pure static page.

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
