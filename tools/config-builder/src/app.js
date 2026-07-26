// DOM glue: the ONLY file that touches the DOM. Forms ⇄ authoring model,
// live validation, model save/load, and config.tar download. All the real
// logic (validate, model→files, tar, uid) lives in the unit-tested pure
// modules imported below.

import { validate } from './validate.js';
import { buildFiles } from './model.js';
import { makeTar } from './tarball.js';
import { decodeCsvBytes, parseDiario, applyDiario } from './diario.js';

const EXAMPLE_URL = new URL('../fixtures/example.model.json', import.meta.url);

// --- authoring model (in-memory; the single source the UI edits) ----------
let model = emptyModel();

function emptyModel() {
  return { capture_photos: false, teachers: [], students: [], classes: [] };
}

// --- boot -----------------------------------------------------------------
init();

async function init() {
  try {
    const res = await fetch(EXAMPLE_URL);
    model = normalize(await res.json());
  } catch {
    // Opened from file:// where fetch of a sibling file may be blocked — start
    // empty; the user can Load a model JSON or add rows by hand.
    model = emptyModel();
  }
  render();
}

// Coerce a loaded model into the shape the editors expect.
function normalize(m) {
  m = m || {};
  return {
    capture_photos: !!m.capture_photos,
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
      teacher_email: c.teacher_email || '', color: c.color || '272766',
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
  const root = document.getElementById('app');
  root.replaceChildren(
    actionBar(),
    toolbar(),
    importSection(),
    optionsSection(),
    teachersSection(),
    studentsSection(),
    classesSection(),
    reviewSection(),
  );
}

// Recompute validity and update the live bits in place (status pill, top
// download button, review card) without disturbing the field being edited.
function syncStatus() {
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
    el('span', { class: 'spacer' }),
    el('button', {
      class: 'small danger', text: 'Reset all fields',
      onclick: () => {
        if (!hasContent() || confirm('Clear all teachers, students, and classes? This cannot be undone (use "Save model JSON" first if unsure).')) {
          model = emptyModel();
          lastImport = null;
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

// --- device options -------------------------------------------------------
function optionsSection() {
  const cb = el('input', { type: 'checkbox', checked: !!model.capture_photos });
  cb.addEventListener('change', () => { model.capture_photos = cb.checked; });
  return el('section', {}, [
    el('h2', {}, ['Device options']),
    el('label', { class: 'switch', style: 'margin-top:14px' }, [cb, el('span', { class: 'track' }), 'Capture a photo on each check-in']),
    el('p', { class: 'opt-desc', text: 'When on, the device saves a snapshot as students check in (needs the camera + SD space).' }),
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
    el('h2', {}, ['Teachers ', el('span', { class: 'count', text: `${model.teachers.length}/8` })]),
    el('p', { class: 'hint', text: 'Each teacher needs a card or a unique digits-only password to sign in.' }),
    body,
    el('button', { class: 'add', text: '+ Add teacher', onclick: () => {
      model.teachers.push({ name: '', email: '', rfid_uid: '', password: '' }); render();
    } }),
  ]);
}

// --- students -------------------------------------------------------------
function studentsSection() {
  const body = model.students.length
    ? el('div', { class: 'rows' }, model.students.map((s, i) =>
        el('div', { class: 'row student' }, [
          fieldInput('University ID', s.id, (v) => { s.id = v; }, { class: 'mono' }),
          fieldInput('Name', s.name, (v) => { s.name = v; }),
          fieldInput('RFID uid', s.rfid_uid, (v) => { s.rfid_uid = v; }, { class: 'mono', placeholder: 'usually blank' }),
          removeBtn('Remove student', () => { model.students.splice(i, 1); render(); }),
        ])))
    : emptyState('No students yet. Import a Diário CSV above, paste an id,name list below, or add rows by hand.');

  const csv = el('textarea', { placeholder: 'Paste "id,name" per line, then Import' });
  return el('section', {}, [
    el('h2', {}, ['Students ', el('span', { class: 'count', text: `${model.students.length}/300` })]),
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
    el('h2', {}, ['Classes ', el('span', { class: 'count', text: `${model.classes.length}/12` })]),
    el('p', { class: 'hint', text: 'A class links to one teacher; students carry their group (turma) per roster entry.' }),
    body,
    el('button', { class: 'add', text: '+ Add class', onclick: () => {
      model.classes.push({ code: '', name: '', schedule: '', teacher_email: '', color: '272766', roster: [] });
      render();
    } }),
  ]);
}

function classCard(c, i) {
  const safeColor = /^[0-9a-fA-F]{6}$/.test(c.color) ? c.color : '272766';

  const teacherSel = el('select', {}, [
    el('option', { value: '', text: '— pick a teacher —' }),
    ...model.teachers.filter((t) => t.email).map((t) =>
      el('option', { value: t.email, text: t.name || t.email })),
  ]);
  teacherSel.value = c.teacher_email;
  teacherSel.addEventListener('change', () => { c.teacher_email = teacherSel.value; syncStatus(); });

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
      el('label', { class: 'field' }, ['Teacher', teacherSel]),
    ]),
    el('label', { class: 'field roster-field' }, [`Roster (${c.roster.length}/100)`, roster]),
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
  const tar = makeTar(buildFiles(model));
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
