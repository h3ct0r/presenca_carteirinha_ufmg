// Pure, DOM-free parser for the UFMG "Diário de Classe" CSV export, plus a
// helper that folds the parsed roster into the authoring model.
//
// The file is semicolon-delimited and looks like:
//
//   PERIODO;ATIVIDADE;TURMA
//   2026/2;DCC219;TE1
//   <blank>
//   N.;MATRICULA;NOME;PTS OBTIDOS
//   1;2025115525;ALIGIA CASSIO DOS SANTOS;0,00
//   2;2025049999;ALINE CRISTINA GONCALVES DA COSTA;0,00
//   ...
//
// We only care about MATRICULA (→ student id) and NOME (→ student name); every
// other column is ignored. The header block gives us ATIVIDADE (course code)
// and TURMA (class group), both surfaced onto the class + student records.
//
// Encoding: these exports are commonly Latin-1 / Windows-1252 (accented
// Portuguese names). decodeCsvBytes() tries strict UTF-8 first and falls back
// to windows-1252, so "CAIO OTÁVIO" survives either way.

const DEFAULT_COLOR = '272766';

// --- decoding -------------------------------------------------------------
export function decodeCsvBytes(buf) {
  const bytes = buf instanceof Uint8Array ? buf : new Uint8Array(buf);
  try {
    return stripBom(new TextDecoder('utf-8', { fatal: true }).decode(bytes));
  } catch {
    // Not valid UTF-8 → assume the Windows/Latin-1 the academic system emits.
    return stripBom(new TextDecoder('windows-1252').decode(bytes));
  }
}

function stripBom(s) {
  return s.charCodeAt(0) === 0xfeff ? s.slice(1) : s;
}

// --- parsing --------------------------------------------------------------
// Accent-insensitive, case-insensitive key for matching HEADER labels only
// (values keep their accents).
function headerKey(s) {
  return s.normalize('NFD').replace(/[̀-ͯ]/g, '').trim().toUpperCase();
}

function cells(line) {
  return line.split(';').map((c) => c.trim());
}

// Parse the CSV text into { periodo, atividade, turma, students:[{id,name}] }.
// Throws if the MATRICULA/NOME data header can't be found.
export function parseDiario(text) {
  const lines = text.split(/\r\n|\r|\n/);
  const rows = lines.map(cells);

  let periodo = '';
  let atividade = '';
  let turma = '';

  // Header meta block: a row labeled PERIODO/ATIVIDADE/TURMA, then its values.
  const metaIdx = rows.findIndex((r) => {
    const keys = r.map(headerKey);
    return keys.includes('ATIVIDADE') || keys.includes('TURMA');
  });
  if (metaIdx >= 0) {
    const keys = rows[metaIdx].map(headerKey);
    const pI = keys.indexOf('PERIODO');
    const aI = keys.indexOf('ATIVIDADE');
    const tI = keys.indexOf('TURMA');
    const valIdx = nextNonEmpty(rows, metaIdx + 1);
    if (valIdx >= 0) {
      const v = rows[valIdx];
      periodo = pI >= 0 ? (v[pI] || '') : '';
      atividade = aI >= 0 ? (v[aI] || '') : '';
      turma = tI >= 0 ? (v[tI] || '') : '';
    }
  }

  // Data header: the row that names MATRICULA and NOME.
  const dataHeaderIdx = rows.findIndex((r) => {
    const keys = r.map(headerKey);
    return keys.includes('MATRICULA') && keys.includes('NOME');
  });
  if (dataHeaderIdx < 0) {
    throw new Error('Not a Diário CSV: no "MATRICULA;NOME" header row found.');
  }
  const hk = rows[dataHeaderIdx].map(headerKey);
  const matIdx = hk.indexOf('MATRICULA');
  const nomeIdx = hk.indexOf('NOME');

  const students = [];
  const seen = new Set();
  let duplicates = 0;
  for (let i = dataHeaderIdx + 1; i < rows.length; i++) {
    const r = rows[i];
    const id = (r[matIdx] || '').trim();
    const name = (r[nomeIdx] || '').trim();
    if (!id || !name) continue; // skip blank lines / footers
    if (seen.has(id)) { duplicates++; continue; }
    seen.add(id);
    students.push({ id, name });
  }

  return { periodo, atividade, turma, students, duplicates };
}

function nextNonEmpty(rows, from) {
  for (let i = from; i < rows.length; i++) {
    if (rows[i].some((c) => c !== '')) return i;
  }
  return -1;
}

// --- apply to the authoring model ----------------------------------------
// Merges a parsed Diário into `model` (mutates it): upserts each student
// (tagging turma) and creates/updates the class. Returns a summary.
export function applyDiario(model, parsed) {
  model.students = model.students || [];
  model.classes = model.classes || [];
  model.teachers = model.teachers || [];

  const turma = parsed.turma || '';
  const code = classCode(parsed.periodo, parsed.atividade);
  const name = parsed.atividade || code;

  // Upsert students into the global registry (no turma here — it's per class).
  let added = 0;
  let updated = 0;
  const byId = new Map(model.students.map((s) => [s.id, s]));
  for (const { id, name: sname } of parsed.students) {
    const existing = byId.get(id);
    if (existing) {
      if (sname) existing.name = sname;
      updated++;
    } else {
      const s = { id, name: sname, rfid_uid: '' };
      model.students.push(s);
      byId.set(id, s);
      added++;
    }
  }

  // Upsert the class, creating it (keyed by code) if it doesn't exist yet.
  let cls = model.classes.find((c) => c.code === code);
  if (!cls) {
    cls = { code, name: '', schedule: '', teacher_email: '', color: '', roster: [] };
    model.classes.push(cls);
  }
  // Fill any blank class fields — on a fresh class every field is blank, and on
  // a re-import this backfills anything the author left empty (never overwrites
  // values they set).
  if (!cls.name) cls.name = name;
  if (!cls.color) cls.color = DEFAULT_COLOR;
  if (!cls.teacher_email) cls.teacher_email = onlyTeacherEmail(model.teachers);
  // Roster entries are { id, turma }. Tag every imported student with this
  // Diário's turma (updating the tag if they're already on the roster).
  const rosterById = new Map(
    cls.roster.map((e) => [typeof e === 'string' ? e : e.id, e]),
  );
  for (const { id } of parsed.students) {
    const entry = rosterById.get(id);
    if (entry && typeof entry === 'object') {
      if (turma) entry.turma = turma;
    } else if (!entry) {
      const e = turma ? { id, turma } : { id };
      cls.roster.push(e);
      rosterById.set(id, e);
    }
  }

  return {
    code, turma, name,
    addedStudents: added,
    updatedStudents: updated,
    total: parsed.students.length,
    duplicates: parsed.duplicates || 0,
  };
}

// classes/<code>/ folder — key by semester + course (<PERIODO>-<ATIVIDADE>, e.g.
// "2026_2-DCC219"), so every turma of the same course+semester merges into ONE
// class; students are distinguished by their per-roster-entry turma. The
// semester's '/' (e.g. "2026/2") is not a legal folder char, so it's replaced
// with '_'; the code stays a safe path segment (validate.js enforces this too).
function classCode(periodo, atividade) {
  const parts = [periodo, atividade].map((p) => (p || '').trim()).filter(Boolean);
  const raw = parts.join('-') || 'IMPORT';
  return raw.replace(/[\\/]/g, '_').replace(/\.\./g, '_');
}

function onlyTeacherEmail(teachers) {
  const withEmail = teachers.filter((t) => t.email);
  return withEmail.length === 1 ? withEmail[0].email : '';
}
