# config-builder — build spec / handoff

An off-device, browser-only tool to author the RFID attendance device's config
and export it as a `.tar` for import. Paste-friendly handoff for a fresh session.

> Read alongside: `CLAUDE.md` (this dir — the rules) and
> `../../docs/software/CONFIG_IMPORT.md` (the authoritative tar/schema contract). This
> spec says *how to build the tool*; the contract says *what the output must be*.

## 1. Objective & non-goals

**Objective:** let a user, on a laptop with no internet, define teachers,
students, and classes; validate them exactly as the device will; and download a
`config.tar` that the device can import (merge) without losing attendance data.

**Non-goals:** editing attendance; talking to the device (v1 downloads a file the
user uploads via the device's existing web file manager — see contract §6);
managing ESP-DL models; being a general SD file browser. *(Student **photos** were
a former non-goal but are now in scope — the tool matches Moodle photos to
students and bundles them into `config.tar`; see `STUDENT_PHOTOS.md` and the
`untar.js` / `photomatch.js` modules.)*

## 2. Why this exists / relationship to the device

The device already configures itself two ways (LVGL admin panel + the on-device
web file editor), but both are awkward for **bulk** roster/class authoring. A
laptop form with real validation is the right place to build a roster of 600
students and 12 classes, and to provision several devices from one authored tree.
The device stays a dumb, safe importer (stage → validate → merge); this tool owns
the authoring UX.

## 3. Tech decision

**Chosen: single self-contained `index.html`, vanilla JS (ES modules), no build,
no CDN. Tests via `node --test`.**

Rationale:
- Matches the project's offline/self-contained ethos and the device's own
  hand-written embedded page. Anyone can open the file; a Claude session can run
  `node --test` with zero install.
- The output is intrinsically "one HTML, one JS, one CSS" with no bundler needed.
- Config authoring is form + validation + a 60-line tar writer — it does not
  need React/Vite/Tailwind. (The original wizard spec proposed those; they add a
  toolchain this repo doesn't have for no proportional benefit.)

**Escalation path (only if justified, record here first):** if the class/roster
editor UX genuinely needs reactive components, adopt **Preact + htm** (no build,
loadable as a local ES module) before considering a bundler. Never add a CDN.

## 4. Proposed structure

```
tools/config-builder/
  CLAUDE.md            # rules (already written)
  SPEC.md              # this file
  README.md            # short human usage (write during M1)
  index.html           # the app shell; inlines styles; imports the ES modules
  DEPLOY.md            # dev server + nginx notes
  index.html           # the app shell; inlines styles; imports the ES modules
  hooks/               # schema-drift guard run from the repo's tooling
  src/
    validate.js        # pure: validate a config model → [errors]; NO DOM
    tarball.js         # pure: files[] → Uint8Array (ustar); NO DOM
    untar.js           # pure: ustar reader (Moodle photo tars); NO DOM
    uid.js             # pure: uid_normalize() port of src/app/uid.cpp
    model.js           # pure: build the JSON docs from the UI model
    diario.js          # pure: parse UFMG Diário CSV → model merge; NO DOM
    photomatch.js      # pure: photo filename → student id matching; NO DOM
    persist.js         # pure: localStorage autosave encode/decode; NO DOM
    app.js             # DOM glue: forms ⇄ model, wires validate + download + import
  test/               # one *.test.js per pure module, plus schema-sync
  fixtures/
    example.model.json # a known-good authored model (mirrors docs/software/sd_card_example)
```

Keep `validate.js`, `tarball.js`, `uid.js`, `model.js` **DOM-free and
side-effect-free** so `node --test` imports them directly. `app.js` is the only
file that touches the DOM.

## 5. Features (screens/sections in one page)

> **Card ids and passwords are authored in the clear here, and that is correct.**
> This tool runs on a laptop and cannot know the device's key. The device
> converts both into keyed fingerprints on the first load and rewrites the file
> (`docs/software/CONFIG_IMPORT.md` §6), and deletes the `config.tar` after a
> successful import. Do NOT try to hash anything in the browser — the result
> would not match any device.

1. **Teachers** — add/remove rows: name, email, rfid_uid (optional), password
   (optional, digits-only). Live per-field + uniqueness validation.
2. **Students** — table: id, name, rfid_uid (default null). Import from
   pasted CSV/TSV is a nice-to-have (id,name per line).
3. **Classes** — per class: code, name, schedule, professors (checkbox list of
   defined teachers), color (picker → 6-hex), roster (multiselect of defined
   student ids). Folder name is derived = code.
4. **Review & Export** — runs full cross-file validation; shows a clear error
   list or a green "ready"; **Download config.tar** button (disabled while
   invalid). Also offer **Download/Load model JSON** so work can be saved and
   resumed (persist the authoring model, not just the tar).

## 6. Validation (from contract §3 — implement in `validate.js`)

Per-field: string length limits, digits-only passwords, 6-hex color, non-empty
id/name/code. Cross-file (the important ones):
- teacher passwords unique (ignoring empties) and digits-only;
- student ids unique; UID uniqueness across students **and** teachers using
  `uid.js` normalization; ≤ 600 students;
- ≤ 12 classes; class `code` unique; every `roster[].id` exists in students;
  ≤ 100 per roster;
- ≥ 1 teacher.

Return structured errors (`{scope, field, message}`) so the UI can point at the
offending row. **Mirror the firmware's messages where practical** so a device
rejection and a builder rejection read the same.

## 7. Tar generation (`tarball.js`)

Emit **uncompressed ustar** (contract §4). Reference implementation:

```js
// files: [{ name: 'config.json', data: Uint8Array|string }, ...]
// returns Uint8Array. Names must already be whitelist-clean (no '..', no leading '/').
export function makeTar(files) {
  const enc = new TextEncoder();
  const blocks = [];
  const pad = (buf) => { const r = buf.length % 512; if (r) blocks.push(new Uint8Array(512 - r)); };
  for (const f of files) {
    const data = typeof f.data === 'string' ? enc.encode(f.data) : f.data;
    const h = new Uint8Array(512);
    const put = (str, off, len) => h.set(enc.encode(str).subarray(0, len), off);
    put(f.name, 0, 100);                       // name
    put('0000644', 100, 7);                    // mode
    put('0000000', 108, 7);                    // uid
    put('0000000', 116, 7);                    // gid
    put(data.length.toString(8).padStart(11, '0'), 124, 11); // size (octal)
    put('00000000000', 136, 11);               // mtime (0 — device has no RTC anyway)
    put('        ', 148, 8);                    // checksum field = spaces during calc
    h[156] = 0x30;                              // typeflag '0' = regular file
    put('ustar', 257, 5); h[263] = 0x30; h[264] = 0x30; // magic "ustar", version "00"
    let sum = 0; for (const b of h) sum += b;   // checksum = sum of header bytes
    put(sum.toString(8).padStart(6, '0') + '\0 ', 148, 8);
    blocks.push(h, data); pad(data);
  }
  blocks.push(new Uint8Array(1024));            // two zero blocks = EOF
  const total = blocks.reduce((n, b) => n + b.length, 0);
  const out = new Uint8Array(total);
  let o = 0; for (const b of blocks) { out.set(b, o); o += b.length; }
  return out;
}
```

Test it by asserting: header magic `ustar`, octal size fields, the checksum
equals the summed header, two trailing zero blocks, and total length is a
multiple of 512. (Optional bonus: shell out to `tar tf` in a test to confirm
real tar accepts it — but keep the primary assertions dependency-free.)

Download via `Blob([tar], {type:'application/x-tar'})` + an `<a download>`.

## 8. Testing plan

`node --test` (zero deps). Minimum:
- `uid.test.js` — normalization matches firmware cases (case-fold, strip `:` `-`
  spaces). Cross-check a couple of values against `src/app/uid.cpp`.
- `validate.test.js` — the fixture passes; each rule fails on a targeted bad
  model (dup id, dup password, non-numeric password, roster id not in students,
  over-cap counts, over-length strings, non-hex color).
- `tarball.test.js` — structural assertions above; round-trip a small file set
  and re-parse the size/name back out.

Run tests after every change (project rule). Report honestly (contract §… /
CLAUDE honesty rule): "validated + unit-tested," not "device-verified," unless a
tar was actually imported by real firmware.

## 9. Milestones

- **M1 — skeleton + tar core:** ✅ done. `tarball.js` (ustar) + `uid.js` (port of
  `src/app/uid.cpp`) + `model.js` (authoring model → the 3 JSON docs), all
  DOM-free; `fixtures/example.model.json`; `index.html`/`app.js` page that loads
  the fixture and downloads a tar. Tar verified with system `tar tf`/extract.
- **M2 — validation:** ✅ done. `validate.js` covers every contract §3 rule
  (structured `{scope,index,field,message}` errors), wired to a live error panel
  that gates the export button. Where the builder is intentionally stricter than
  the device's *load* validator (length limits; student↔teacher UID collision;
  teacher_email must resolve; a teacher must be reachable), each rule is marked
  `[builder-stricter]` in the source and follows the contract.
- **M3 — authoring UI:** *mostly done* — teachers/students/classes editors,
  roster checkboxes, teacher dropdown, color picker, save/load model JSON all
  work. Remaining polish: keep input focus on re-render of the edited section
  (today only the export panel refreshes live; structural add/remove re-renders
  the whole page), per-field inline error highlighting.
- **M4 — polish:** CSV paste for students ✅; color picker ✅; **Diário de Classe
  import** ✅; **Moodle photo ingestion** ✅; **localStorage autosave** ✅
  (`src/persist.js`); **per-class check-in settings** ✅. Remaining (optional):
  empty states, print/QR of the AP upload steps.

### Diário de Classe importer (`src/diario.js`)
Imports the UFMG semicolon CSV export. `parseDiario(text)` reads the header
(`PERIODO;ATIVIDADE;TURMA`) and the `N.;MATRICULA;NOME;PTS OBTIDOS` table,
keeping **only** matricula→id and nome→name. `decodeCsvBytes(bytes)` tries UTF-8
then falls back to **windows-1252** so accented names survive Latin-1 exports.
`applyDiario(model, parsed)` upserts students (global registry, **no** turma) and
creates the class `<SEMESTER>-<ATIVIDADE>` (e.g. `2026_2-DCC219`; semester `/` →
`_`), tagging each **roster entry** with the Diário's turma — so every turma of
the same course+semester merges into one class. Header matching is accent/case-
insensitive; students dedupe by matricula. Pure + DOM-free; the UI (import
section, file+paste, per-roster turma inputs) lives in `app.js`.

**`turma` schema field:** an optional per-student tag on each **`class.json`
roster entry** (`{ "id", "turma" }`, ≤ 15 chars) — **not** on the student
registry, **not** class-level. So one class can hold multiple turmas and a
student can differ per class. See `docs/software/CONFIG_IMPORT.md` §3.3 and its changelog.
The firmware reads only `id` from a roster entry and preserves `turma` on rewrite
(no `roster.h` struct change needed). Validated in `validate.js` (`ROSTER_TURMA`).

**Per-class check-in settings** (2026-07-30): each class carries a **check-in
mode** — single tap, double tap, or photo check-in — plus `min_attendance_min`
(default 45) and `face_verify_seconds` (default 15). All four are emitted on
every class, so an import states them rather than leaving the device defaults.
The device treats double-tap and photo check-in as independent flags and can
combine them; this picker cannot. See `docs/software/CONFIG_IMPORT.md` §3.3.

**Tests:** `node --test`, green — the count lives in [`README.md`](README.md).
`schema-sync.test.js` parses the firmware headers and fails if `validate.js`'s
limits drift from them.

## 10. Definition of done (v1)

Offline single page that: builds a valid model, blocks export while invalid with
pointed errors, and downloads a `config.tar` that a human can upload via the
device file manager. All pure modules unit-tested green. No CDN, no build, no
network. `docs/software/CONFIG_IMPORT.md` and this tool agree.

## 11. Open questions

- **Existing-device merge preview:** should the tool ingest a device-exported
  current config to diff against? (Nice, not v1.)

Resolved: the upload path is `/config.tar` at the SD root, reachable by copying
the file or by the device's `POST /api/upload` — no `POST /api/import` was
needed (contract §6). localStorage autosave shipped (`src/persist.js`).
