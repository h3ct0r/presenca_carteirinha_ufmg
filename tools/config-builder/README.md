# config-builder

Offline, browser-only tool to author the RFID attendance device's configuration
(teachers, students, classes) and export it as a `config.tar` the device can
import. Runs on a laptop — **not** on the ESP32.

- **Rules & conventions:** [`CLAUDE.md`](CLAUDE.md)
- **Build plan / handoff:** [`SPEC.md`](SPEC.md)
- **Output format (the contract):** [`../../docs/CONFIG_IMPORT.md`](../../docs/CONFIG_IMPORT.md)
- **Deploying it (dev server + nginx):** [`DEPLOY.md`](DEPLOY.md)

Status: **M1 + M2 done** (tar/uid/model core, full validation, and a working
authoring page); M3/M4 polish ongoing. See `SPEC.md` §9 for milestones.

## Quick use
1. Open `index.html` in a browser (no install, no internet). It loads the
   example roster (`fixtures/example.model.json`) so you can see the shape.
   *(When opened straight from `file://`, the browser may block the example
   fetch — use **Load model JSON** or add rows by hand; a `python3 -m
   http.server` in this dir avoids that.)*
2. *(optional)* **Import Diário de Classe (CSV)** — pick the UFMG semicolon CSV
   export and it fills the students and a class automatically (see below).
3. Fill in teachers, students, and classes; fix any validation errors listed in
   the **Review & export** panel. **Save model JSON** to resume later.
4. Click **Download config.tar** (enabled only when there are no errors).
5. Connect to the device's WiFi AP and upload the tar via the device's web file
   manager (see the contract for the import flow).

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
- upserts by matricula, so re-importing updates instead of duplicating.

You still assign a **teacher** to the imported class before exporting (a lone
defined teacher is auto-assigned). `turma` lives on each `class.json` roster
entry (`{ "id", "turma" }`) — not on the student registry — so one class can span
turmas and a student can carry a different turma per class. See
`docs/CONFIG_IMPORT.md` §3.3. Roster turmas are editable inline per member.

## Develop
- **Run / host:** it's a static single-page app (no build, no backend). Serve it
  with `python3 -m http.server` for development or nginx for production — see
  [`DEPLOY.md`](DEPLOY.md).
- **Test:** `node --test` (zero deps) — 50 cases across `uid`, `tarball`,
  `validate`, `diario`. Run after every change (project rule).
- **Layout:** pure, DOM-free modules in `src/` (`uid.js`, `tarball.js`,
  `model.js`, `validate.js`, `diario.js`) are unit-tested headlessly;
  `src/app.js` is the only file that touches the DOM.
