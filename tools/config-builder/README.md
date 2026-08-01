# config-builder

Offline, browser-only tool to author the RFID attendance device's configuration
(teachers, students, classes) and export it as a `config.tar` the device can
import. Runs on a laptop — **not** on the ESP32.

- **Sample inputs to try it with:** [`docs/software/build_tool_example/`](../../docs/software/build_tool_example/)
- **Rules & conventions:** [`CLAUDE.md`](CLAUDE.md)
- **Build plan / handoff:** [`SPEC.md`](SPEC.md)
- **Output format (the contract):** [`../../docs/software/CONFIG_IMPORT.md`](../../docs/software/CONFIG_IMPORT.md)
- **Deploying it (dev server + nginx):** [`DEPLOY.md`](DEPLOY.md)

Status: **complete and in use** — tar/uid/model core, full validation, the
authoring page, Diário CSV import, Moodle photo ingestion, per-class check-in
settings, and localStorage autosave. See [`SPEC.md`](SPEC.md) §9 for the
milestone record and what is deliberately left out.

## Quick use
1. Open `index.html` in a browser (no install, no internet). It starts **empty**
   — click **Load example** if you want to see the shape of a filled-in roster.
   *(The example is fetched, which the browser may block on `file://`; serving
   the folder with `python3 -m http.server` avoids that. Everything else works
   from `file://`.)*
   Your work is **auto-saved in this browser** (localStorage) after every edit,
   so reloading or reopening the page continues where you left off — the toolbar
   shows when it was last saved. **Reset all fields** clears that copy too.
   Student *photos* are not saved (they are binary and large): re-import the
   Moodle tar each session. For a portable/backed-up copy, still use
   **Save model JSON**.
2. *(optional)* **Import Diário de Classe (CSV)** — pick the UFMG semicolon CSV
   export and it fills the students and a class automatically (see below).
3. Fill in teachers, students, and classes; fix any validation errors listed in
   the **Review & export** panel. **Save model JSON** to resume later.
3a. *(optional)* **Student photos (Moodle)** — drop the Moodle photos `.tar`; it
   matches each photo to a student by name, re-keys it to matrícula, re-encodes
   it to a baseline 100×100 JPEG, and bundles it into `config.tar`. Confirm any
   "needs review"/unmatched photos in the picker. Photos are **not** saved in the
   model JSON — re-import the tar each session (see
   [`STUDENT_PHOTOS.md`](../../docs/software/STUDENT_PHOTOS.md)).
3b. Each class carries a **Check-in mode** — *single tap*, *double tap*
   (students tap on arrival and again once the threshold has passed, default
   **45 min**), or *photo check-in* (the kiosk verifies a face and saves a photo,
   default **15 s** to capture). These are written into every `class.json`, so
   **importing a tar overwrites whatever was set in the device's ⚙ class
   settings**. The device can combine double-tap with photo check-in; this
   picker cannot — set that combination on the device.
4. Click **Download config.tar** (enabled only when there are no errors).
5. Connect to the device's WiFi AP and upload the tar via the device's web file
   manager (see the contract for the import flow). That path needs the device
   already configured — the file manager is behind the unlock gate. For a device
   whose card is blank, either copy the tar to the card root directly (the idle
   screen then offers **Import config from SD**) or use **Set up this device** on
   the idle screen to create a first professor; see
   [`SD_CARD.md`](../../docs/software/SD_CARD.md#preparing-a-new-card).

## Diário de Classe import
The UFMG "Diário de Classe" export is a semicolon CSV with a header block
(`PERIODO;ATIVIDADE;TURMA`) and a `N.;MATRICULA;NOME;PTS OBTIDOS` table. Import
**one or several files at once** — pick multiple in the file dialog or **drag &
drop** them onto the import box; each file is reported on its own summary line
(classes of the same course+semester merge). The importer:
- reads **only** `MATRICULA` (→ student id) and `NOME` (→ name); everything else
  is ignored;
- handles **accented Portuguese names** and **Latin-1/Windows-1252** files
  (tries UTF-8, falls back automatically);
- takes `ATIVIDADE` + `TURMA` from the header and creates a class
  `<SEMESTER>-<ATIVIDADE>` (e.g. `2026_2-DCC219`; the semester `/` becomes `_`),
  enrolling every imported student and tagging each **roster entry** with its
  `turma`. All turmas of the same course+semester merge into that one class;
- upserts by matricula, so re-importing updates instead of duplicating;
- gives each new class a **random colour** from a built-in palette, skipping
  colours already used by other classes so they stay easy to tell apart on the
  device. Every palette entry is dark enough for the white class initial drawn
  on top (WCAG AA). A colour you pick yourself is never overwritten, and
  re-importing a class keeps the colour it already has.

You still assign the **professors** to the imported class before exporting — each
class card has a checkbox list of the defined teachers, and a class may be
**co-taught by several** (it then shows for each of them on the device). A lone
defined teacher is auto-assigned. `turma` lives on each `class.json` roster
entry (`{ "id", "turma" }`) — not on the student registry — so one class can span
turmas and a student can carry a different turma per class. See
[`CONFIG_IMPORT.md`](../../docs/software/CONFIG_IMPORT.md) §3.3. Roster turmas are
editable inline per member.

## Develop
- **Run / host:** it's a static single-page app (no build, no backend). Serve it
  with `python3 -m http.server` for development or nginx for production — see
  [`DEPLOY.md`](DEPLOY.md).
- **Test:** `node --test` (zero deps) — 135 cases across `uid`, `tarball`,
  `untar`, `validate`, `model.color`, `diario`, `photomatch`, `persist`,
  `checkin`, and the `schema-sync` drift guard. Run after every change (project
  rule).
- **Layout:** pure, DOM-free modules in `src/` (`uid.js`, `tarball.js`,
  `untar.js`, `model.js`, `validate.js`, `diario.js`, `photomatch.js`,
  `persist.js`) are
  unit-tested headlessly; `src/app.js` is the only file that touches the DOM.
