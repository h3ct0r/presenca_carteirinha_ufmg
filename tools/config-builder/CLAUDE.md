# CLAUDE.md — config-builder (off-device tool)

**This is NOT firmware.** It is a browser-only tool that runs on a laptop and
produces a `.tar` of authored config for the RFID attendance device to import.
The embedded-C++/LVGL/PlatformIO rules in the repo root `CLAUDE.md` do **not**
apply here — these do. Read `SPEC.md` (this directory) for the full build plan
and `../../docs/software/CONFIG_IMPORT.md` for the tar contract.

## What it is
A single, self-contained, offline HTML page. The user fills in teachers,
students, and classes; the page validates them and downloads a `.tar` matching
the contract. No device connection, no backend, no internet.

## Hard rules (match the project's ethos)
- **Offline, no CDN, no network.** Every asset inlined/vendored locally. The
  page must work opened from `file://` with no connectivity. (Same rule the
  device's embedded page follows.)
- **No build step, no framework, no bundler** to start. Vanilla JS + one
  self-contained `index.html`. Reach for a framework only if the form UX truly
  outgrows vanilla — and record the decision in `SPEC.md` first.
- **The contract is `../../docs/software/CONFIG_IMPORT.md`.** Schemas, limits, and the
  path whitelist come from there. Do not invent fields. If firmware changes the
  schema, that doc changes first; keep this tool in lockstep.
- **Validate to the contract before emitting a tar.** The whole point is to fail
  on the laptop, with a clear message, instead of on the device. Cross-file
  checks (roster ids exist in students.json; unique/normalized UIDs; unique
  digits-only passwords) are mandatory, not just per-field checks.
- **Never emit a path outside the authored whitelist** (`config.json`,
  `students/students.json`, `classes/<CODE>/class.json`). No `attendance/`, no
  `..`, no leading `/`. The device rejects them; don't produce them.
- **UID normalization must match the firmware** (`../../src/app/uid.cpp`:
  uppercase hex, separators stripped) or the builder's uniqueness check disagrees
  with the device.

## Run & test
- **Run:** open `index.html` in a browser (or `SPEC.md`'s dev-server note). No
  install.
- **Test:** `node --test` (Node's built-in runner — **zero dependencies**). Put
  the validation + tar-writing logic in plain `.js` modules importable by both
  the page and the tests, so logic is unit-tested headlessly. Run tests after
  every change (mirrors the firmware's "test after every change" rule).

## Honesty rule
Report changes as **"validated against docs/software/CONFIG_IMPORT.md and unit-tested,"**
never as "verified against the device" unless you actually round-tripped a tar
through real firmware. (The maintainer does test each feature on the device
before moving on — but that is their step, not yours to claim.)

## Keeping in sync (read this before schema work)
The single source of truth is `../../docs/software/CONFIG_IMPORT.md`, which itself mirrors
`../../src/services/{roster,config}_service.cpp`. The order for any schema change:
1. change the firmware validator, 2. update `docs/software/CONFIG_IMPORT.md`
(+ changelog), 3. update this tool + its tests. A PR/change touching one without
the others is incomplete.
