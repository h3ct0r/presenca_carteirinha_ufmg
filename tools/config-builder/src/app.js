// DOM glue: the ONLY file that touches the DOM. Forms ⇄ authoring model,
// live validation, model save/load, and config.tar download. All the real
// logic (validate, model→files, tar, uid) lives in the unit-tested pure
// modules imported below.

import { validate, LIMITS, CHECKIN_MODES, checkinMode } from './validate.js';
import {
  buildFiles, classTeacherEmails, DEFAULT_FACE_VERIFY_SECONDS, DEFAULT_MIN_ATTENDANCE_MIN,
} from './model.js';
import { makeTar } from './tarball.js';
import { decodeCsvBytes, parseDiario, applyDiario } from './diario.js';
import { parseTar } from './untar.js';
import { matchPhotos } from './photomatch.js';
import {
  STORAGE_KEY, encodeModel, decodeModel, modelHasContent, describeSavedAt,
} from './persist.js';
import { versionLabel } from './version.js';

const EXAMPLE_URL = new URL('../fixtures/example.model.json', import.meta.url);

// --- authoring model (in-memory; the single source the UI edits) ----------
let model = emptyModel();

// Local-persistence state. Declared HERE, above the init() call below: `let` is
// not hoisted, and init() synchronously reaches render() -> saveLocalSoon(),
// so declaring these further down puts them in the temporal dead zone and the
// whole boot fails with a ReferenceError swallowed by the async function.
let lastSavedAt = 0;
let saveTimer = null;
let saveError = '';

function emptyModel() {
  return { teachers: [], students: [], classes: [] };
}

// Boot with a CLEAN sheet — never the bundled example. If the browser kept a
// working copy from a previous visit, restore that instead so a reload (or an
// accidental tab close) doesn't lose the work. "Load example" is still one click
// away in the toolbar.
async function init() {
  showVersion();
  const restored = loadLocal();
  if (restored) {
    model = normalize(restored.model);
    lastSavedAt = restored.savedAt;
  } else {
    model = emptyModel();
  }
  render();
}

// Build id in the header. Written once at boot — it never changes — and guarded
// because the header lives in index.html: a trimmed deployment (DEPLOY.md allows
// shipping a minimal bundle) must not take the whole app down over a label.
function showVersion() {
  const slot = document.getElementById('brand-version');
  if (slot) slot.textContent = versionLabel();
}

// --- local working copy (survives a reload) --------------------------------
// localStorage can be unavailable or full (private windows, quota, some
// file:// setups). Persistence is a convenience, never a requirement: every
// failure degrades to "not saved" and is surfaced in the toolbar hint rather
// than breaking the page.
function loadLocal() {
  try {
    return decodeModel(window.localStorage.getItem(STORAGE_KEY));
  } catch {
    return null;  // storage blocked entirely
  }
}

function writeLocal() {
  saveTimer = null;
  try {
    if (!modelHasContent(model)) {
      // Nothing worth keeping (e.g. right after Reset): drop the record so the
      // next visit starts clean instead of restoring an empty shell.
      window.localStorage.removeItem(STORAGE_KEY);
      lastSavedAt = 0;
    } else {
      const now = Date.now();
      window.localStorage.setItem(STORAGE_KEY, encodeModel(model, now));
      lastSavedAt = now;
    }
    saveError = '';
  } catch (e) {
    saveError = e && e.name === 'QuotaExceededError' ? 'browser storage is full'
                                                     : 'browser storage unavailable';
  }
  refreshSavedHint();
}

// Debounced: editing a field fires on every keystroke, and serializing a
// 600-student model that often would be wasteful.
function saveLocalSoon() {
  if (saveTimer) clearTimeout(saveTimer);
  saveTimer = setTimeout(writeLocal, 400);
}

function clearLocal() {
  if (saveTimer) { clearTimeout(saveTimer); saveTimer = null; }
  try { window.localStorage.removeItem(STORAGE_KEY); } catch { /* nothing to do */ }
  lastSavedAt = 0;
  saveError = '';
}

function savedHintText() {
  if (saveError) return `Not saved — ${saveError}`;
  if (!modelHasContent(model)) return 'Nothing to save yet';
  if (!lastSavedAt) return 'Saving…';
  return `Saved in this browser · ${describeSavedAt(lastSavedAt)}`;
}

function refreshSavedHint() {
  const el = document.querySelector('.saved-hint');
  if (el) {
    el.textContent = savedHintText();
    el.classList.toggle('bad', !!saveError);
  }
}

// Coerce a loaded model into the shape the editors expect.
function normalize(m) {
  m = m || {};
  return {
    teachers: (m.teachers || []).map((t) => ({
      name: t.name || '', email: t.email || '',
      rfid_uid: t.rfid_uid || '', password: t.password == null ? '' : String(t.password),
    })),
    students: (m.students || []).map((s) => ({
      id: s.id || '', name: s.name || '',
      rfid_uid: s.rfid_uid == null ? '' : String(s.rfid_uid),
    })),
    classes: (m.classes || []).map((c) => ({
      code: c.code || '', name: c.name || '', schedule: c.schedule || '',
      teacher_emails: classTeacherEmails(c), color: c.color || '272766',
      // Per-class attendance settings (contract §3.3). Absent in an older model
      // JSON means "never authored" — fall back to the contract defaults, which
      // are what the device would have used anyway.
      capture_photos: !!c.capture_photos,
      timed_attendance: !!c.timed_attendance,
      face_verify_seconds: c.face_verify_seconds ?? DEFAULT_FACE_VERIFY_SECONDS,
      min_attendance_min: c.min_attendance_min ?? DEFAULT_MIN_ATTENDANCE_MIN,
      // Roster entries are { id, turma }. Accept bare-id strings and {id} too.
      roster: (c.roster || [])
        .map((r) => (typeof r === 'string' ? { id: r, turma: '' } : { id: r.id || '', turma: r.turma || '' }))
        .filter((e) => e.id),
    })),
  };
}

// --- tiny DOM helpers -----------------------------------------------------
function el(tag, attrs = {}, kids = []) {
  const n = document.createElement(tag);
  for (const [k, v] of Object.entries(attrs)) {
    if (k === 'class') n.className = v;
    else if (k === 'text') n.textContent = v;
    else if (k.startsWith('on')) n.addEventListener(k.slice(2), v);
    else if (v !== null && v !== undefined && v !== false) n.setAttribute(k, v === true ? '' : v);
  }
  for (const kid of [].concat(kids)) if (kid || kid === 0) n.append(kid);
  return n;
}

// A labelled text input that live-validates the model on every keystroke.
function fieldInput(labelText, value, oninput, attrs = {}) {
  const input = el('input', { value: value ?? '', ...attrs });
  input.addEventListener('input', () => { oninput(input.value); syncStatus(); });
  return el('label', { class: 'field' }, [labelText, input]);
}

function removeBtn(label, onclick) {
  return el('button', { class: 'icon danger rm', title: label, 'aria-label': label, onclick }, ['✕']);
}

// Feather-style eye / eye-off icons (static markup — safe as innerHTML).
const EYE_SVG = '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.9" stroke-linecap="round" stroke-linejoin="round"><path d="M1.5 12S5 5 12 5s10.5 7 10.5 7-3.5 7-10.5 7S1.5 12 1.5 12Z"/><circle cx="12" cy="12" r="3"/></svg>';
const EYE_OFF_SVG = '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.9" stroke-linecap="round" stroke-linejoin="round"><path d="M10.6 5.2A9.9 9.9 0 0 1 12 5c7 0 10.5 7 10.5 7a17.8 17.8 0 0 1-2.4 3.3M6.2 6.2A17.5 17.5 0 0 0 1.5 12s3.5 7 10.5 7a9.7 9.7 0 0 0 4.2-.9"/><path d="M9.9 9.9a3 3 0 0 0 4.2 4.2"/><line x1="2" y1="2" x2="22" y2="22"/></svg>';

// A masked field (shows •••) with an eye toggle to reveal/hide the value.
function passwordField(labelText, value, oninput, attrs = {}) {
  const input = el('input', { type: 'password', value: value ?? '', ...attrs });
  input.addEventListener('input', () => { oninput(input.value); syncStatus(); });

  const toggle = el('button', { type: 'button', class: 'eye', 'aria-label': 'Show password', title: 'Show password' });
  toggle.innerHTML = EYE_SVG;
  toggle.addEventListener('click', (e) => {
    e.preventDefault(); e.stopPropagation();
    const shown = input.type === 'password';
    input.type = shown ? 'text' : 'password';
    toggle.innerHTML = shown ? EYE_OFF_SVG : EYE_SVG;
    const lbl = shown ? 'Hide password' : 'Show password';
    toggle.setAttribute('aria-label', lbl); toggle.setAttribute('title', lbl);
    input.focus();
  });

  return el('label', { class: 'field' }, [labelText, el('div', { class: 'input-wrap' }, [input, toggle])]);
}

function emptyState(text) {
  return el('div', { class: 'empty', text });
}

// --- render ---------------------------------------------------------------
function render() {
  saveLocalSoon();  // structural change (rows added/removed, import, load)
  const root = document.getElementById('app');
  root.replaceChildren(
    actionBar(),
    toolbar(),
    importSection(),
    teachersSection(),
    studentsSection(),
    photosSection(),
    classesSection(),
    reviewSection(),
  );
}

// Recompute validity and update the live bits in place (status pill, top
// download button, review card) without disturbing the field being edited.
// `refreshTeachers` is false when the caller IS a professor checkbox: rebuilding
// the list under a click would swap out the element mid-event and drop focus.
function syncStatus(refreshTeachers = true) {
  const errors = validate(model);
  const ok = errors.length === 0 && hasContent();
  const pill = document.querySelector('.statuspill');
  if (pill) {
    pill.className = 'statuspill pill ' + (ok ? 'ok' : 'bad');
    pill.querySelector('.pill-text').textContent = statusText(ok, errors.length);
  }
  const dlTop = document.getElementById('dl-top');
  if (dlTop) dlTop.disabled = !ok;
  const review = document.getElementById('review');
  if (review) review.replaceWith(reviewSection());
  if (refreshTeachers) refreshTeacherSelects();
  saveLocalSoon();  // keep the browser copy in step with every edit
}

// Rebuild each class's professor checkbox list from the current teachers, in
// place, so a teacher added or edited while a class card is open shows up
// immediately (and renamed teachers update their label) — without re-rendering
// the field the user is typing in. Each box carries its class index, so this
// does not depend on DOM order.
function refreshTeacherSelects() {
  document.querySelectorAll('.teacher-box').forEach((box) => {
    const cls = model.classes[Number(box.dataset.classIndex)];
    if (cls) renderTeacherBox(box, cls);
  });
}

// Fills a class card's professor checkbox list. Rebuilt whenever the teacher
// list changes, so newly added/renamed professors appear without a full render.
function renderTeacherBox(box, c) {
  if (!model.teachers.length) {
    box.replaceChildren(el('div', { class: 'teacher-empty', text: 'Add a teacher first.' }));
    return;
  }
  const chosen = classTeacherEmails(c);
  // List EVERY teacher, including ones with no email yet. A class links to a
  // teacher by email, so an email-less teacher cannot be attached — but hiding
  // them made the box read "Add a teacher first." while teachers plainly
  // existed, leaving the "no professor selected" error impossible to clear.
  // Show them disabled with the reason instead, so the fix is obvious.
  box.replaceChildren(...model.teachers.map((t, ti) => {
    const label = t.name || t.email || `Teacher ${ti + 1}`;
    if (!t.email) {
      return el('label', { class: 'teacher-row disabled' }, [
        el('input', { type: 'checkbox', disabled: true }),
        el('span', {}, [
          label,
          el('span', { class: 'teacher-need', text: ' — add an email above to assign' }),
        ]),
      ]);
    }
    const cb = el('input', { type: 'checkbox', checked: chosen.includes(t.email) });
    cb.addEventListener('change', () => {
      const next = classTeacherEmails(c).filter((e) => e !== t.email);
      if (cb.checked) next.push(t.email);
      c.teacher_emails = next;
      delete c.teacher_email;  // normalized away from the legacy scalar
      syncStatus(false);       // don't rebuild this list under the click
    });
    return el('label', { class: 'teacher-row' }, [cb, label]);
  }));
}

function statusText(ok, n) {
  if (ok) return 'Ready to export';
  if (!hasContent()) return 'Nothing to export yet';
  return `${n} issue${n === 1 ? '' : 's'} to fix`;
}

// Sticky bar: live status (jumps to the review card) + the primary Download.
function actionBar() {
  const errors = validate(model);
  const ok = errors.length === 0 && hasContent();
  const pill = el('button', {
    class: 'statuspill pill ' + (ok ? 'ok' : 'bad'),
    title: 'Jump to the review',
    onclick: () => document.getElementById('review')?.scrollIntoView(),
  }, [el('span', { class: 'dot' }), el('span', { class: 'pill-text', text: statusText(ok, errors.length) })]);

  return el('div', { class: 'actionbar' }, [
    pill,
    el('span', { class: 'spacer' }),
    el('button', {
      id: 'dl-top', class: 'primary', text: 'Download config.tar',
      onclick: downloadTar, disabled: !ok,
    }),
  ]);
}

// Model-wide file actions, grouped; destructive Reset pushed to the right.
function toolbar() {
  return el('div', { class: 'toolbar' }, [
    el('button', { class: 'small', text: 'Load example', onclick: loadExample }),
    loadModelButton(),
    el('button', { class: 'small', text: 'Save model JSON', onclick: saveModel }),
    el('span', { class: 'saved-hint', text: savedHintText() }),
    el('span', { class: 'spacer' }),
    el('button', {
      class: 'small danger', text: 'Reset all fields',
      onclick: () => {
        if (!hasContent() || confirm('Clear all teachers, students, and classes? This cannot be undone (use "Save model JSON" first if unsure).')) {
          model = emptyModel();
          lastImport = null;
          clearLocal();  // otherwise a reload would bring it all back
          render();
        }
      },
    }),
  ]);
}

async function loadExample() {
  try { model = normalize(await (await fetch(EXAMPLE_URL)).json()); render(); }
  catch { alert('Could not load the example (fetch is blocked on file://). Use "Load model JSON", or run a local server.'); }
}

// --- Diário de Classe import ---------------------------------------------
// Result of the last import, shown in the panel (survives the re-render).
let lastImport = null; // { ok: bool, text: string }

// Apply one CSV's text to the model, returning a one-line summary (label is
// the file name, or '' for pasted text). Throws on a parse failure.
function importText(text, label) {
  const r = applyDiario(model, parseDiario(text));
  const who = label ? `${label} → ` : '';
  return `✓ ${who}${r.code} (turma ${r.turma || '—'}) — ${r.addedStudents} added, ${r.updatedStudents} updated`
    + (r.duplicates ? `, ${r.duplicates} dup skipped` : '');
}

// Import one or more Diário CSV files (from the picker or a drop). Reads each as
// bytes so Latin-1/Windows-1252 exports keep their accents; classes merge by
// code, so several files of the same course+semester fold into one class.
async function importFiles(fileList) {
  const files = Array.from(fileList || []).filter((f) => f);
  if (!files.length) return;
  const lines = [];
  let anyError = false;
  for (const f of files) {
    try {
      lines.push(importText(decodeCsvBytes(await f.arrayBuffer()), f.name));
    } catch (e) {
      anyError = true;
      lines.push(`✕ ${f.name}: ${e.message}`);
    }
  }
  if (!anyError && model.teachers.length !== 1) {
    lines.push('Remember to assign a teacher to the imported class(es) below.');
  }
  lastImport = { ok: !anyError, text: lines.join('\n') };
  render();
}

function importSection() {
  const paste = el('textarea', { placeholder: 'Paste one Diário CSV here, then Import pasted text' });
  const runPaste = () => {
    try { lastImport = { ok: true, text: importText(paste.value, '') }; }
    catch (e) { lastImport = { ok: false, text: '✕ ' + e.message }; }
    render();
  };

  // Multi-file picker.
  const file = el('input', {
    type: 'file', accept: '.csv,text/csv,text/plain', multiple: true, class: 'hidden-dl',
  });
  file.addEventListener('change', () => { importFiles(file.files); file.value = ''; });

  // Drop zone (drag & drop, multiple files).
  const dz = el('div', { class: 'dropzone' }, [
    el('button', { class: 'small primary', text: 'Choose CSV file(s)…', onclick: () => file.click() }),
    file,
    el('span', { class: 'dz-text', text: 'or drag & drop CSV files here' }),
  ]);
  const stop = (e) => { e.preventDefault(); e.stopPropagation(); };
  dz.addEventListener('dragenter', (e) => { stop(e); dz.classList.add('dragover'); });
  dz.addEventListener('dragover', (e) => { stop(e); dz.classList.add('dragover'); });
  dz.addEventListener('dragleave', (e) => { stop(e); dz.classList.remove('dragover'); });
  dz.addEventListener('drop', (e) => { stop(e); dz.classList.remove('dragover'); importFiles(e.dataTransfer && e.dataTransfer.files); });

  const result = lastImport
    ? el('div', { class: 'import-result ' + (lastImport.ok ? 'ok' : 'bad'), text: lastImport.text })
    : null;

  return el('section', {}, [
    el('h2', {}, ['Import Diário de Classe']),
    el('p', { class: 'hint', text:
      'UFMG semicolon CSV. Each file fills the students and one class from its header '
      + '(keyed SEMESTER-ATIVIDADE); only MATRÍCULA and NOME are read. Accented names and '
      + 'Latin-1 files are handled. Select or drop several at once.' }),
    el('div', { style: 'margin-top:16px' }, [dz]),
    el('details', {}, [
      el('summary', { text: 'Paste CSV text instead' }),
      paste,
      el('div', { style: 'margin-top:8px' }, [el('button', { class: 'small', text: 'Import pasted text', onclick: runPaste })]),
    ]),
    result,
  ]);
}

// --- teachers -------------------------------------------------------------
function teachersSection() {
  const body = model.teachers.length
    ? el('div', { class: 'rows' }, model.teachers.map((t, i) =>
        el('div', { class: 'row teacher' }, [
          fieldInput('Name', t.name, (v) => { t.name = v; }),
          fieldInput('Email', t.email, (v) => { t.email = v; }, { type: 'email' }),
          fieldInput('RFID uid', t.rfid_uid, (v) => { t.rfid_uid = v; }, { class: 'mono', placeholder: 'optional' }),
          passwordField('Password', t.password, (v) => { t.password = v; }, { class: 'mono', inputmode: 'numeric', placeholder: 'digits' }),
          removeBtn('Remove teacher', () => { model.teachers.splice(i, 1); render(); }),
        ])))
    : emptyState('No teachers yet. Add at least one — a teacher signs in with an RFID card or a unique digits-only password. (Teachers aren’t in the Diário CSV, so add them here.)');

  return el('section', {}, [
    el('h2', {}, ['Teachers ', el('span', { class: 'count', text: `${model.teachers.length}/${LIMITS.MAX_TEACHERS}` })]),
    el('p', { class: 'hint', text: 'Each teacher needs a card or a unique digits-only password to sign in.' }),
    body,
    el('button', { class: 'add', text: '+ Add teacher', onclick: () => {
      model.teachers.push({ name: '', email: '', rfid_uid: '', password: '' }); render();
    } }),
  ]);
}

// --- students -------------------------------------------------------------
// Filters the student list by id/name; module-level so it survives re-renders.
let studentFilter = '';

function studentsSection() {
  let body;
  if (model.students.length) {
    const shown = el('span', { class: 'search-count' });
    const search = el('input', {
      type: 'search', class: 'student-search', value: studentFilter,
      placeholder: 'Search students by ID or name…', 'aria-label': 'Search students by ID or name',
    });

    // Each row tags itself with a searchable "id name" string; the filter just
    // hides non-matches in place, so typing keeps focus (no re-render).
    const rows = model.students.map((s, i) => {
      const row = el('div', { class: 'row student' });
      const tag = () => { row.dataset.search = `${s.id} ${s.name}`.toLowerCase(); };
      row.append(
        fieldInput('University ID', s.id, (v) => { s.id = v; tag(); applyStudentFilter(); }, { class: 'mono' }),
        fieldInput('Name', s.name, (v) => { s.name = v; tag(); applyStudentFilter(); }),
        fieldInput('RFID uid', s.rfid_uid, (v) => { s.rfid_uid = v; }, { class: 'mono', placeholder: 'usually blank' }),
        removeBtn('Remove student', () => { model.students.splice(i, 1); render(); }),
      );
      tag();
      return row;
    });
    const list = el('div', { class: 'student-list' }, rows);

    function applyStudentFilter() {
      const q = studentFilter.trim().toLowerCase();
      let n = 0;
      for (const row of rows) {
        const hit = !q || row.dataset.search.includes(q);
        row.style.display = hit ? '' : 'none';
        if (hit) n++;
      }
      shown.textContent = q ? `${n} of ${rows.length} shown` : '';
    }
    search.addEventListener('input', () => { studentFilter = search.value; applyStudentFilter(); });
    applyStudentFilter();

    body = el('div', {}, [el('div', { class: 'search-row' }, [search, shown]), list]);
  } else {
    studentFilter = '';
    body = emptyState('No students yet. Import a Diário CSV above, paste an id,name list below, or add rows by hand.');
  }

  const csv = el('textarea', { placeholder: 'Paste "id,name" per line, then Import' });
  return el('section', {}, [
    el('h2', {}, ['Students ', el('span', { class: 'count', text: `${model.students.length}/${LIMITS.MAX_STUDENTS}` })]),
    el('p', { class: 'hint', text: 'Cards are normally bound on the device at first tap — leave RFID uid blank unless you already know it.' }),
    body,
    el('button', { class: 'add', text: '+ Add student', onclick: () => {
      model.students.push({ id: '', name: '', rfid_uid: '' }); render();
    } }),
    el('details', {}, [
      el('summary', { text: 'Paste an id,name list' }),
      csv,
      el('div', { style: 'margin-top:8px' }, [el('button', { class: 'small', text: 'Import rows', onclick: () => { importStudentsCsv(csv.value); render(); } })]),
    ]),
  ]);
}

// --- student photos (Moodle) ----------------------------------------------
// Session-scoped side store (NOT saved in model.json — re-import each session;
// see STUDENT_PHOTOS.md §7). Each item: { filename, bytes, url, id, reason }.
//   reason: 'exact' | 'suggested' | 'ambiguous' | 'unmatched'
//   id:     the resolved student id, or '' when unassigned/skipped.
let photoItems = [];
let photoNote = null;  // { ok, text } status line from the last import.

function basename(path) {
  return String(path).split('/').pop();
}

// Decode a JPEG and re-encode it as a baseline 100×100 JPEG via a canvas
// (guarantees baseline for the device's HW decoder, strips EXIF; STUDENT_PHOTOS
// §6). Falls back to the original bytes if the browser can't decode it.
function normalizeJpeg(bytes) {
  return new Promise((resolve) => {
    const url = URL.createObjectURL(new Blob([bytes], { type: 'image/jpeg' }));
    const img = new Image();
    img.onload = () => {
      try {
        const c = document.createElement('canvas');
        c.width = 100; c.height = 100;
        c.getContext('2d').drawImage(img, 0, 0, 100, 100);
        c.toBlob(
          (blob) => {
            URL.revokeObjectURL(url);
            if (!blob) return resolve(bytes);
            blob.arrayBuffer().then((ab) => resolve(new Uint8Array(ab))).catch(() => resolve(bytes));
          },
          'image/jpeg', 0.85,
        );
      } catch { URL.revokeObjectURL(url); resolve(bytes); }
    };
    img.onerror = () => { URL.revokeObjectURL(url); resolve(bytes); };
    img.src = url;
  });
}

const IMG_RE = /\.(jpe?g|png|gif|webp|bmp)$/i;

// Parse one or more Moodle photo tars, normalise each image, match by name to
// the roster, and populate photoItems + a summary. Merges across tars/re-imports
// (same basename → replaced).
async function importPhotoTars(fileList) {
  const files = Array.from(fileList || []).filter((f) => f);
  if (!files.length) return;
  if (!model.students.length) {
    photoNote = { ok: false, text: '✕ Add or import students first — photos are matched to the roster by name.' };
    render();
    return;
  }

  // filename(basename) → raw bytes, across all selected tars.
  const raw = new Map();
  let tars = 0, badTars = 0, skipped = 0;
  for (const f of files) {
    try {
      for (const e of parseTar(await f.arrayBuffer())) {
        const bn = basename(e.name);
        // Skip macOS archive cruft (AppleDouble sidecars, __MACOSX/, hidden files)
        // and non-images — the "._Name.jpg" sidecars would otherwise match a
        // student by name and clobber the real avatar bytes.
        if (bn.startsWith('.') || e.name.includes('__MACOSX/') || !IMG_RE.test(bn)) {
          skipped++;
          continue;
        }
        raw.set(bn, e.data);
      }
      tars++;
    } catch { badTars++; }
  }

  // Preserve any prior manual assignments for filenames we're re-importing.
  const priorId = new Map(photoItems.map((it) => [it.filename, it.id]));

  // Normalise every image (in parallel) and build id-keyed lookup for matching.
  const names = [...raw.keys()];
  const normalised = await Promise.all(names.map((n) => normalizeJpeg(raw.get(n))));
  const bytesByName = new Map(names.map((n, i) => [n, normalised[i]]));

  const { matched, review, unmatched } = matchPhotos(names, model.students);
  const items = [];
  const mk = (filename, id, reason) => {
    const bytes = bytesByName.get(filename);
    return {
      filename, bytes, reason,
      id: priorId.has(filename) ? priorId.get(filename) : id,
      url: URL.createObjectURL(new Blob([bytes], { type: 'image/jpeg' })),
    };
  };
  for (const m of matched) items.push(mk(m.filename, m.id, 'exact'));
  for (const r of review) items.push(mk(r.filename, r.suggestion || '', r.reason));
  for (const u of unmatched) items.push(mk(u.filename, '', 'unmatched'));

  // Free the object URLs from any previous import before replacing the list.
  for (const it of photoItems) if (it.url) URL.revokeObjectURL(it.url);
  photoItems = items;

  const parts = [`✓ ${raw.size} photo(s) from ${tars} tar(s)`];
  if (badTars) parts.push(`${badTars} file(s) not a readable tar`);
  if (skipped) parts.push(`${skipped} non-image entr(y/ies) skipped`);
  photoNote = { ok: badTars === 0, text: parts.join(' · ') };
  render();
}

// Photos ready to export (assigned to a real roster id), deduped by id.
function exportablePhotos() {
  const ids = new Set(model.students.map((s) => s.id));
  const byId = new Map();
  for (const it of photoItems) if (it.id && ids.has(it.id)) byId.set(it.id, it.bytes);
  return [...byId.entries()].map(([id, data]) => ({ name: `students/photos/${id}.jpg`, data }));
}

function photosSection() {
  const assigned = photoItems.filter((it) => it.id).length;
  const needReview = photoItems.filter((it) => !it.id && it.reason !== 'unmatched').length
    + photoItems.filter((it) => it.reason === 'suggested' && it.id).length;
  const unmatched = photoItems.filter((it) => !it.id && it.reason === 'unmatched').length;
  const withoutPhoto = model.students.filter(
    (s) => !photoItems.some((it) => it.id === s.id)).length;

  // File picker + drop zone (mirrors the Diário importer).
  const file = el('input', { type: 'file', accept: '.tar,application/x-tar', multiple: true, class: 'hidden-dl' });
  file.addEventListener('change', () => { importPhotoTars(file.files); file.value = ''; });
  const dz = el('div', { class: 'dropzone' }, [
    el('button', { class: 'small primary', text: 'Choose photo tar(s)…', onclick: () => file.click() }),
    file,
    el('span', { class: 'dz-text', text: 'or drag & drop Moodle photo .tar files here' }),
  ]);
  const stop = (e) => { e.preventDefault(); e.stopPropagation(); };
  dz.addEventListener('dragenter', (e) => { stop(e); dz.classList.add('dragover'); });
  dz.addEventListener('dragover', (e) => { stop(e); dz.classList.add('dragover'); });
  dz.addEventListener('dragleave', (e) => { stop(e); dz.classList.remove('dragover'); });
  dz.addEventListener('drop', (e) => { stop(e); dz.classList.remove('dragover'); importPhotoTars(e.dataTransfer && e.dataTransfer.files); });

  const note = photoNote
    ? el('div', { class: 'import-result ' + (photoNote.ok ? 'ok' : 'bad'), text: photoNote.text })
    : null;

  const kids = [
    el('h2', {}, ['Student photos (Moodle) ',
      photoItems.length ? el('span', { class: 'count', text: `${assigned}/${photoItems.length}` }) : null]),
    el('p', { class: 'hint', text:
      'Upload the Moodle photos .tar (100×100 JPEGs named by student). They are matched '
      + 'to the roster by name, re-keyed to matrícula, re-encoded to baseline JPEG, and '
      + 'bundled into config.tar. Not saved in model JSON — re-import each session.' }),
    el('div', { style: 'margin-top:16px' }, [dz]),
    note,
  ];

  if (photoItems.length) {
    kids.push(el('div', { class: 'photo-summary', text:
      `${assigned} assigned · ${needReview} need review · ${unmatched} unmatched · ${withoutPhoto} student(s) without a photo` }));
    kids.push(photoReviewList());
  }
  return el('section', {}, kids);
}

// A row per photo that isn't an exact auto-match: thumbnail + a student picker.
function photoReviewList() {
  const needing = photoItems.filter((it) => it.reason !== 'exact');
  if (!needing.length) {
    return el('p', { class: 'hint', text: '✓ Every uploaded photo matched a student exactly.' });
  }
  const studentOptions = (selected) => [
    el('option', { value: '', text: '— skip —' }),
    ...model.students.map((s) =>
      el('option', { value: s.id, text: `${s.name} (${s.id})`, selected: s.id === selected || null })),
  ];
  const rows = needing.map((it) => {
    const sel = el('select', { class: 'photo-pick' }, studentOptions(it.id));
    sel.value = it.id;
    sel.addEventListener('change', () => { it.id = sel.value; render(); });
    const badge = it.reason === 'ambiguous' ? 'ambiguous' : it.reason === 'suggested' ? 'suggested' : 'no match';
    return el('div', { class: 'photo-row' }, [
      el('img', { src: it.url, class: 'photo-thumb', alt: it.filename, width: 48, height: 48 }),
      el('div', { class: 'photo-meta' }, [
        el('div', { class: 'photo-name', text: it.filename }),
        el('span', { class: 'tag', text: badge }),
      ]),
      sel,
    ]);
  });
  return el('div', { class: 'photo-review' }, rows);
}

function importStudentsCsv(text) {
  for (const line of text.split(/\r?\n/)) {
    const trimmed = line.trim();
    if (!trimmed) continue;
    const parts = trimmed.split(/[,\t]/).map((p) => p.trim());
    const [id, ...rest] = parts;
    if (!id) continue;
    model.students.push({ id, name: rest.join(' ').trim(), rfid_uid: '' });
  }
}

// --- classes --------------------------------------------------------------
function classesSection() {
  const body = model.classes.length
    ? el('div', { class: 'rows' }, model.classes.map((c, i) => classCard(c, i)))
    : emptyState('No classes yet. Import a Diário CSV above (it creates the class and enrolls its students), or add one by hand.');

  return el('section', {}, [
    el('h2', {}, ['Classes ', el('span', { class: 'count', text: `${model.classes.length}/${LIMITS.MAX_CLASSES}` })]),
    el('p', { class: 'hint', text: 'A class links to one teacher; students carry their group (turma) per roster entry.' }),
    body,
    el('button', { class: 'add', text: '+ Add class', onclick: () => {
      model.classes.push({
        code: '', name: '', schedule: '', teacher_emails: [], color: '272766',
        capture_photos: false, timed_attendance: false,
        face_verify_seconds: DEFAULT_FACE_VERIFY_SECONDS,
        min_attendance_min: DEFAULT_MIN_ATTENDANCE_MIN,
        roster: [],
      });
      render();
    } }),
  ]);
}

function classCard(c, i) {
  const safeColor = /^[0-9a-fA-F]{6}$/.test(c.color) ? c.color : '272766';

  // A class may be co-taught, so professors are a checkbox list (not a single
  // pick). Selections live in c.teacher_emails; the legacy scalar is folded in
  // by classTeacherEmails() and dropped on the first edit.
  const teacherBox = el('div', { class: 'teacher-box' });
  teacherBox.dataset.classIndex = String(i);
  renderTeacherBox(teacherBox, c);

  // The header swatch IS the color picker — tap it to recolor the class.
  const color = el('input', { type: 'color', class: 'swatch-input', value: '#' + safeColor, title: 'Class color', 'aria-label': 'Class color' });
  color.addEventListener('input', () => { c.color = color.value.replace('#', ''); });

  const roster = el('div', { class: 'roster-box' },
    model.students.length
      ? model.students.map((s) => {
          const entry = c.roster.find((e) => e.id === s.id);
          const cb = el('input', { type: 'checkbox', checked: !!entry });
          const turma = el('input', {
            class: 'roster-turma', value: entry ? entry.turma : '', placeholder: 'turma', disabled: !entry,
          });
          turma.addEventListener('input', () => { const e = c.roster.find((x) => x.id === s.id); if (e) e.turma = turma.value; });
          cb.addEventListener('change', () => {
            if (cb.checked) { if (!c.roster.find((e) => e.id === s.id)) c.roster.push({ id: s.id, turma: '' }); }
            else c.roster = c.roster.filter((e) => e.id !== s.id);
            render();
          });
          return el('label', { class: 'roster-row' }, [
            cb,
            el('span', { class: 'roster-name' }, [el('span', { class: 'rid', text: s.id }), ` · ${s.name || '(no name)'}`]),
            turma,
          ]);
        })
      : [el('div', { class: 'roster-empty', text: 'Add students first, then tick them here.' })]);

  const codeInput = el('input', { class: 'mono', value: c.code });
  const titleEl = el('span', { class: 'classcard-title', text: c.code || 'New class' });
  codeInput.addEventListener('input', () => { c.code = codeInput.value; titleEl.textContent = codeInput.value || 'New class'; syncStatus(); });

  return el('div', { class: 'classcard' }, [
    el('div', { class: 'classcard-head' }, [color, titleEl, removeBtn('Remove class', () => { model.classes.splice(i, 1); render(); })]),
    el('div', { class: 'class-grid' }, [
      el('label', { class: 'field' }, ['Code (folder name)', codeInput]),
      fieldInput('Name', c.name, (v) => { c.name = v; }),
      fieldInput('Schedule', c.schedule, (v) => { c.schedule = v; }, { placeholder: 'e.g. Tue/Thu 10:00' }),
      el('label', { class: 'field' }, ['Professors', teacherBox]),
    ]),
    checkinField(c),
    el('label', { class: 'field roster-field' },
      [`Roster (${c.roster.length}/${LIMITS.MAX_CLASS_ROSTER})`, roster]),
  ]);
}

// How students register presence in this class. The device stores two
// independent booleans, but authoring one mode per class keeps the choice
// legible — see CONFIG_IMPORT.md §3.3 for the mapping and its one limitation.
const CHECKIN_LABELS = {
  single: ['Single tap', 'One tap on the reader marks the student present.'],
  double: ['Double tap', 'Tap on arrival, then tap again once the threshold has passed.'],
  photo: ['Photo check-in', 'The kiosk verifies a face and saves a photo for each tap.'],
};

function checkinField(c) {
  const mode = checkinMode(c);

  const select = el('select', { class: 'checkin-mode' },
    Object.keys(CHECKIN_MODES).map((key) =>
      el('option', { value: key, selected: key === mode }, [CHECKIN_LABELS[key][0]])));
  select.addEventListener('change', () => {
    // Assign the pair the mode stands for; the two numbers are kept either way
    // so flipping modes back and forth never loses a typed value.
    Object.assign(c, CHECKIN_MODES[select.value]);
    render();
  });

  // Only the number the chosen mode actually uses is shown — the other one is
  // still emitted (at its stored value), it just has no effect on the device.
  const extra = [];
  if (mode === 'double') {
    extra.push(numberField('Minutes before the second tap counts', c.min_attendance_min,
      DEFAULT_MIN_ATTENDANCE_MIN, LIMITS.MIN_ATTENDANCE_MIN, LIMITS.MIN_ATTENDANCE_MAX,
      (v) => { c.min_attendance_min = v; }));
  } else if (mode === 'photo') {
    extra.push(numberField('Seconds to capture the photo', c.face_verify_seconds,
      DEFAULT_FACE_VERIFY_SECONDS, LIMITS.FACE_VERIFY_SECONDS_MIN,
      LIMITS.FACE_VERIFY_SECONDS_MAX, (v) => { c.face_verify_seconds = v; }));
  }

  return el('div', { class: 'checkin-box' }, [
    el('div', { class: 'class-grid' }, [
      el('label', { class: 'field' }, ['Check-in mode', select]),
      ...extra,
    ]),
    el('p', { class: 'hint checkin-hint', text: CHECKIN_LABELS[mode][1] }),
  ]);
}

// A labelled integer input. Blank restores `fallback` in the model, so a class
// can never end up with an empty setting in the tar.
function numberField(labelText, value, fallback, min, max, onchange) {
  const input = el('input', {
    type: 'number', class: 'num', value: value ?? fallback,
    min: String(min), max: String(max), step: '1',
  });
  input.addEventListener('input', () => {
    onchange(input.value === '' ? fallback : Number(input.value));
    syncStatus();
  });
  return el('label', { class: 'field' }, [
    labelText,
    input,
    el('span', { class: 'field-note', text: `${min}–${max}, default ${fallback}` }),
  ]);
}

// --- review & export ------------------------------------------------------
function reviewSection() {
  const errors = validate(model);
  const ok = errors.length === 0 && hasContent();

  const body = ok
    ? el('div', { class: 'review-ready' }, ['✓ Everything checks out — download the config.tar and upload it to the device.'])
    : errors.length
      ? el('ul', { class: 'errors' }, errors.map((e) =>
          el('li', {}, [el('span', { class: 'tag', text: e.scope }), el('span', { text: e.message })])))
      : el('p', { class: 'hint', text: 'Add teachers, students, and classes (or import a Diário) to get started.' });

  return el('section', { id: 'review' }, [
    el('h2', {}, ['Review & export', errors.length ? el('span', { class: 'count', text: String(errors.length) }) : null]),
    body,
    el('div', { class: 'review-actions' }, [
      el('button', { class: 'primary', text: 'Download config.tar', onclick: downloadTar, disabled: !ok }),
      el('button', { class: 'small', text: 'Save model JSON', onclick: saveModel }),
    ]),
  ]);
}

function hasContent() {
  return model.teachers.length > 0 || model.students.length > 0 || model.classes.length > 0;
}

// --- file I/O -------------------------------------------------------------
function downloadTar() {
  const tar = makeTar([...buildFiles(model), ...exportablePhotos()]);
  triggerDownload(new Blob([tar], { type: 'application/x-tar' }), 'config.tar');
}

function saveModel() {
  const json = JSON.stringify(model, null, 2) + '\n';
  triggerDownload(new Blob([json], { type: 'application/json' }), 'config.model.json');
}

function loadModelButton() {
  const input = el('input', { type: 'file', accept: '.json,application/json', class: 'hidden-dl' });
  input.addEventListener('change', async () => {
    const file = input.files && input.files[0];
    if (!file) return;
    try { model = normalize(JSON.parse(await file.text())); render(); }
    catch (e) { alert('Could not read that model JSON: ' + e.message); }
  });
  return el('span', {}, [el('button', { class: 'small', text: 'Load model JSON', onclick: () => input.click() }), input]);
}

function triggerDownload(blob, filename) {
  const url = URL.createObjectURL(blob);
  const a = el('a', { href: url, download: filename, class: 'hidden-dl' });
  document.body.append(a);
  a.click();
  a.remove();
  setTimeout(() => URL.revokeObjectURL(url), 0);
}

// --- boot -------------------------------------------------------------------
// LAST statement on purpose: init() renders immediately (no await before the
// first render), and render() reads module state declared throughout this file.
// Calling it any earlier evaluates those `let` bindings in their temporal dead
// zone and the page dies with a ReferenceError swallowed by the async function.
init();
