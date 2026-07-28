# Student Photos (Moodle) → Device — Design & Implementation Plan

**Status:** partially built. The **device display side (decision #4) is
implemented** (2026-07-27): when a student registers presence in a class
session, the check-in feedback overlay shows their avatar from
`/students/photos/<id>.jpg` if it exists — success *or* failure — falling back to
a placeholder otherwise. Rendering uses the LVGL TJPGD decoder (`LV_USE_TJPGD`)
over a read-only SD filesystem driver on drive `S`
([`src/ui/lvgl_fs_sd.cpp`](../../src/ui/lvgl_fs_sd.cpp)); the path helper is
[`src/ui/components/student_photo.cpp`](../../src/ui/components/student_photo.cpp)
and the overlay lives in the session tab of
[`src/ui/screens/scr_class.cpp`](../../src/ui/screens/scr_class.cpp). **Build-
verified, not run on hardware.** **The ingestion side is NOT built** — nothing
yet gets photos onto the card (the config-builder does not parse the Moodle tar,
re-key name→id, or bundle photos into `config.tar`; the importer's §4 whitelist
does not yet allow `students/photos/**`). Until that lands there are no avatars
to show, so the device always falls back to the placeholder. The rest of this
doc is the plan for that ingestion work; follow the firmware-first sync order
(contract → firmware → tool) and report changes as *build/test-verified, not run
on hardware* until proven on the device.

Related: [`CONFIG_IMPORT.md`](CONFIG_IMPORT.md) (the tar contract this extends),
[`PROJECT_HANDOFF.md`](PROJECT_HANDOFF.md) (project state), the config-builder in
[`../../tools/config-builder/`](../../tools/config-builder/).

---

## 1. Goal & agreed decisions

Professors download a `.tar` of student photos from the Moodle instructor UI.
Each file is a **100×100 JPEG** whose **filename is the student's name in caps
with underscores for spaces** (e.g. `MARIA_SANTOS.jpg`, `CAIO_OTAVIO_MESSIAS.jpg`).
We want those photos to reach the device and be shown when a student registers
their presence.

Decisions locked in with the user:

1. **One combined `config.tar`.** Photos ship *inside* the same tar the
   config-builder already produces, landing on the SD card **beside**
   `students.json`.
2. **The browser (config-builder) does all the work:** parse the Moodle tar,
   match each photo to a student, and **rename it from name → matrícula (id)** for
   the export. The device receives only id-named files and never matches names.
3. **Photos are already 100×100 and small** (~3–8 KB each), so **no resizing is
   needed** — at most a light re-encode to normalise the format (§6).
4. **Device use:** when a student registers presence, the **success screen shows
   that student's photo if it exists on the SD card**; otherwise it falls back to
   today's success screen.

---

## 2. End-state SD layout

Photos live under `/students/`, keyed by **matrícula (id)** — the stable unique
key the device already uses — literally beside `students.json`:

```
/students/
  students.json
  photos/
    2025115525.jpg          ← <id>.jpg, re-keyed by the builder
    2018107393.jpg
    ...
```

This is the `/students/photos/<id>.jpg` path already noted as *planned* in the SD
data model. It is **distinct** from `/photos/**`, which are the device's own
check-in *snapshots* (written by `photo_store`, gated by the per-class photo
option). Those
two trees must not be conflated:

| Path | Origin | On import |
|------|--------|-----------|
| `/students/photos/<id>.jpg` | **authored** (this feature) | **overwrite** (create dir if new) |
| `/photos/**` | device snapshots | **preserve — never touch** |

---

## 3. Tar contract changes (`CONFIG_IMPORT.md`)

Extend the authored tar (currently `config.json`, `students/students.json`,
`classes/<CODE>/class.json`) with a fourth authored kind:

- **Whitelist (§4):** additionally accept
  `students/photos/<seg>.jpg` where `<seg>` has no `/` or `..`, and `<seg>` should
  be a student id present in `students.json` (unknown-id photos → warn & skip on
  import, or reject; decide in §11).
- **Merge rule (§2):** `students/photos/<id>.jpg` is **authored / overwrite**;
  `/photos/**` stays **preserve**.
- **Size cap (§4):** the current suggested **≤ 1 MB** is the blocker — hundreds of
  photos are several MB. Raise it (proposal: **≤ 16 MB**) *and* require the
  importer to **stream the tar to an SD staging file** (never buffer in the 128 KB
  LVGL heap). SD has the room; RAM is the constraint.
- **Changelog + version:** photos are additive (a device that ignores them still
  works); `students.json` `version` need not bump. Record the whitelist/cap change
  in the contract changelog.

The contract's own note already anticipates this: *"[uncompressed] gzip buys
nothing … Revisit only if photos are ever bundled — currently they are not."*
With multi-MB payloads, **reconsider gzip** for the photo tar to cut transfer
time over the AP (tradeoff: a decompressor on the device). Default recommendation:
stay uncompressed (JPEGs are already compressed; gzip saves little) unless AP
transfer proves slow.

---

## 4. Config-builder pipeline (browser, offline)

New user flow, as its own section in the page ("Student photos (Moodle)"):

1. **Upload** the Moodle photos `.tar` (file picker + drag-drop, like the Diário
   import). Multiple tars allowed (merge).
2. **Parse** the tar in-browser → `[{ filename, bytes }]`. We have a ustar
   *writer* (`tarball.js`); add a small **ustar reader** (new pure module,
   `src/untar.js`, DOM-free, unit-tested) — parse 512-byte headers, read `name`
   + octal `size`, slice the JPEG bytes, skip padding/EOF blocks. (Alternative:
   accept a dropped **folder** of JPGs via `webkitdirectory` and skip tar parsing
   — but the user has a tar, so parse it.)
3. **Match** each filename → student id (§5), with a **review UI** (§5).
4. **Normalise the image** (§6) — optional light re-encode; keep 100×100.
5. **Hold** matched photos in an in-memory store `Map<id, Uint8Array>` alongside
   the authoring model (photos are binary; they do **not** go into `model.json` —
   see §7).
6. **Export:** `makeTar()` gains the extra entries `students/photos/<id>.jpg`
   from that store, so **Download config.tar** now carries the photos too.

New/changed files (pure logic stays DOM-free + `node --test`ed):
- `src/untar.js` — ustar reader. **New. Unit-test it** (round-trip against
  `makeTar`, real-tar fixtures).
- `src/photomatch.js` — name normalisation + filename→id matching + report.
  **New. Unit-test it** against realistic name pairs (accents, particles,
  duplicates, unmatched).
- `src/model.js` / a new packing step — add `students/photos/<id>.jpg` entries to
  the file list `makeTar()` receives.
- `src/app.js` — the photos section, the review UI, wire the store into export.
- `validate.js` — optional: warn if a photo has no matching student, or a student
  id used as a photo name isn't in the roster (belt-and-suspenders; the matching
  step already guarantees this).

The **schema-sync limits are unaffected** (no `LIMITS` change), so that drift
guard/test stays green.

---

## 5. Matching algorithm (name → id) + review UX

The photo filename and the student's `name` both originate from the **same UFMG
source** (the Diário `NOME` we already import), so name↔name matching should hit
high. Never match on the device.

**Canonical key** (apply to both the filename-without-extension and each
`student.name`):

```
NFD-decompose → strip combining marks (deaccent)
uppercase
replace '_' and runs of whitespace with a single space
drop non [A-Z0-9 ] (punctuation)
trim
```

Optionally also build a **looser key** that removes Portuguese name particles
(`DE DA DO DAS DOS E`) to catch "ANA DA SILVA" vs "ANA SILVA".

**Procedure:**
1. Build `nameKey(student.name) → [id, …]` over the roster (a list, to detect
   duplicate names).
2. For each photo: `nameKey(filename)` → look up.
   - **exact, unique** → auto-match (high confidence).
   - **exact, multiple students** → *ambiguous* → review.
   - **no exact** → fuzzy fallback (token-set overlap or Levenshtein on the joined
     key); if best candidate ≥ threshold → *suggested* → review; else *unmatched*.

**Review UI (essential — matching is never 100%):**
- Buckets: **Matched** (auto), **Needs review** (ambiguous/suggested, with the
  photo thumbnail + a student dropdown to confirm/override), **Unmatched photos**,
  **Students without a photo**.
- A running summary (e.g. "412 matched · 6 need review · 3 unmatched").
- Manual assign/skip; nothing exports until the reviewer is satisfied.

**Filename hygiene:** strip the extension; guard against Moodle trailing junk
(`_1`, numeric suffixes, assignment ids). The review step is the safety net.

---

## 6. Image handling

Photos are already **100×100**, so **no resize**. Two choices:

- **Pass-through** (smallest change): store the original JPEG bytes, rename to
  `<id>.jpg`. Simplest; relies on Moodle JPEGs being **baseline** (the P4 hardware
  JPEG decoder does not do progressive).
- **Normalise (recommended):** decode → draw to a 100×100 `<canvas>` → `toBlob`
  JPEG (~q0.85). Guarantees **baseline JPEG**, strips EXIF/orientation, and
  bounds size. Costs a light recompression. Given the device decoder's pickiness,
  this is the safer default.

Either way, keep the device-facing images tiny.

---

## 7. Model persistence

`model.json` (Save/Load model JSON) stays **JSON-only and small** — it does **not**
embed photo bytes. Photos are a **session-scoped side store**: re-import the
Moodle tar each session (or when photos change). Document this clearly in the UI
(e.g. "Photos aren't saved in model JSON — re-import the tar to include them").
*(If persistence is ever wanted, add a separate `photos.model.json` of base64
blobs — but that bloats the portable model; default is re-import.)*

---

## 8. Firmware changes

Two independent pieces; both are greenfield (the import endpoint doesn't exist
yet, and nothing displays per-student photos today).

**A. Import endpoint (`services/…`, per `CONFIG_IMPORT.md` §5):**
- Stream the tar to `/import_staging/`, apply the §4 whitelist (now incl.
  `students/photos/<seg>.jpg`).
- Move authored files into place with the atomic `temp → remove → rename` pattern;
  create `/students/photos/` if new; **overwrite** photos; **preserve**
  `/photos/**` and attendance.
- Back up first (`backup_store_create()`), validate JSON, discard staging on any
  failure. Photos with no matching student id → skip + warn (or reject; §11).

**B. Success-screen display:**
- On a successful presence registration for student `id`, build
  `/students/photos/<id>.jpg`, check existence, and if present show it on the
  success overlay; else fall back to the current success screen (name + check).
- Screens involved: **kiosk self-check-in** success overlay (`scr_kiosk.cpp`,
  the full-screen green result) and the **roll-call** confirmation
  (`scr_class.cpp`). Decide whether both show the photo or just kiosk (§11).
- **LVGL image-from-SD is non-trivial:** needs an LVGL filesystem driver mapped
  to the SD **and** a JPEG **decoder** for display. The board has a **hardware
  JPEG codec** (already used by `photo_store` to *encode*); wiring a decode path
  (either an LVGL image-decoder plugin, or decode into an RGB565 buffer and show
  via `lv_image`/`lv_canvas`) is the real firmware effort here. Watch the LVGL
  heap (`LV_MEM_SIZE` 128 KB) — decode into a PSRAM buffer, not the LVGL heap.
- No RAM to hold all photos; load one on demand per check-in and free it on hide.

---

## 9. Size budget

100×100 baseline JPEG ≈ **3–8 KB**. Realistic rosters:

| Students | ~Photo payload | Combined tar |
|---------:|---------------:|-------------:|
| 300 | ~1.5–2.4 MB | < 3 MB |
| 600 | ~3–5 MB | < 6 MB |

Comfortably under a **16 MB** cap, far over the current **1 MB** — hence the cap
raise + streaming import in §3. All of it fits easily on the SD card.

---

## 10. Phased implementation plan

1. **Contract** — update `CONFIG_IMPORT.md`: whitelist `students/photos/<id>.jpg`,
   raise the cap, add the merge-rule row + changelog. *(Contract-first per the
   sync rule.)*
2. **Builder, pure core** — `untar.js` + `photomatch.js` with `node --test`
   suites (tar round-trip; matching against realistic name pairs incl. accents,
   particles, duplicates, misses).
3. **Builder, UI** — photos upload section + matching review UI; wire the photo
   store into `makeTar()` export. Verify the combined `config.tar` extracts with
   system `tar` and contains `students/photos/<id>.jpg`.
4. **Firmware import** — the streaming import endpoint incl. photos (this is the
   larger, still-unbuilt config-import endpoint).
5. **Firmware display** — SD→LVGL JPEG decode + show on the success screen, with
   fallback when absent.

Steps 1–3 are self-contained and de-risk matching quality + sizing on real data
**without any firmware work**. Steps 4–5 are the device-side lift.

---

## 11. Open questions / decisions still to make

- **Unknown-id photo** (filename matches nobody / id not in `students.json`):
  builder skips it in review; should the device importer **skip+warn** or
  **reject** the import? (Proposal: skip+warn.)
- **Which success screens** show the photo — kiosk only, or roll-call too?
- **Normalise vs pass-through** JPEGs (§6). (Proposal: normalise, for baseline
  guarantee.)
- **Gzip** the (now multi-MB) tar for faster AP upload? (Proposal: no, revisit if
  slow.)
- **Photo dimensions on screen** — the device shows 480×800 portrait; is 100×100
  the display size, or upscaled? Affects whether Moodle 100×100 is enough or we
  should ask for larger source images.
- **Delete semantics** — if a student is removed or re-imported, should a stale
  `students/photos/<id>.jpg` be cleaned up? (Import overwrites but won't delete
  orphans; a separate cleanup is out of scope for v1.)

---

## 12. Note on scope

This intentionally **expands a former non-goal** — the config-builder SPEC lists
"managing photos" under non-goals. That line should be updated when this lands.
The device side (import endpoint + JPEG-on-SD display) is the bulk of the effort
and is entirely new firmware.
