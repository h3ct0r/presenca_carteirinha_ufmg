import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { LIMITS } from '../src/validate.js';

// Schema drift guard. The firmware is the source of truth for the on-card
// format (docs/software/CONFIG_IMPORT.md restates it; this tool validates against it).
// This test parses the firmware headers and asserts the builder's LIMITS still
// match — so if someone changes a buffer size or a cap on the device and forgets
// the tool/contract, `node --test` goes red. See tools/config-builder/CLAUDE.md
// ("Keeping in sync") for the required change order.

const root = (p) => readFileSync(fileURLToPath(new URL('../../../' + p, import.meta.url)), 'utf8');
const rootOpt = (p) => { try { return root(p); } catch { return null; } };

const rosterH = root('include/app/roster.h');
const teacherH = root('include/app/teacher.h');
const configH = root('include/services/config_service.h');
// The contract doc is the sync target but may be relocated (docs/ is being
// restructured). Treat it as optional: if it isn't found, the doc-mirror check
// skips instead of crashing the whole suite — the firmware↔validate.js checks
// (the load-bearing ones) still run. Repoint this path if the doc moves.
const contract = rootOpt('docs/software/CONFIG_IMPORT.md');

// --- tiny C-header parsers ------------------------------------------------
function structBlock(src, name) {
  const m = src.match(new RegExp('typedef struct\\s*{([\\s\\S]*?)}\\s*' + name + '\\s*;'));
  assert.ok(m, `struct ${name} not found`);
  return m[1];
}
function charBuf(block, field) {
  const m = block.match(new RegExp('char\\s+' + field + '\\s*\\[\\s*(\\d+)\\s*\\]'));
  assert.ok(m, `char ${field}[] not found`);
  return Number(m[1]);
}

// A 2-D buffer like `char teacher_emails[ROSTER_MAX_CLASS_TEACHERS][64]` — the
// per-entry length is the LAST dimension (the first may be a named constant).
function charBuf2d(block, field) {
  const m = block.match(new RegExp('char\\s+' + field + '\\s*\\[[^\\]]*\\]\\s*\\[\\s*(\\d+)\\s*\\]'));
  assert.ok(m, `char ${field}[][] not found`);
  return Number(m[1]);
}
function constInt(src, name) {
  const m = src.match(new RegExp(name + '\\s*=\\s*(\\d+)'));
  assert.ok(m, `${name} not found`);
  return Number(m[1]);
}

const teacher = structBlock(teacherH, 'teacher_t');
const student = structBlock(rosterH, 'student_t');
const klass = structBlock(rosterH, 'class_rec_t');

// LIMIT key  ->  firmware buffer size (max chars = buffer - 1).
const LENGTHS = {
  TEACHER_NAME: charBuf(teacher, 'name'),
  TEACHER_EMAIL: charBuf(teacher, 'email'),
  TEACHER_UID: charBuf(teacher, 'rfid_uid'),
  TEACHER_PASSWORD: charBuf(teacher, 'password'),
  STUDENT_ID: charBuf(student, 'id'),
  STUDENT_NAME: charBuf(student, 'name'),
  STUDENT_UID: charBuf(student, 'rfid_uid'),
  CLASS_CODE: charBuf(klass, 'code'),
  CLASS_NAME: charBuf(klass, 'name'),
  CLASS_SCHEDULE: charBuf(klass, 'schedule'),
  CLASS_TEACHER_EMAIL: charBuf2d(klass, 'teacher_emails'),
};

// LIMIT key  ->  firmware cap constant (equal).
const COUNTS = {
  MAX_TEACHERS: constInt(configH, 'CONFIG_MAX_TEACHERS'),
  MAX_STUDENTS: constInt(rosterH, 'ROSTER_MAX_STUDENTS'),
  MAX_CLASSES: constInt(rosterH, 'ROSTER_MAX_CLASSES'),
  MAX_CLASS_ROSTER: constInt(rosterH, 'ROSTER_MAX_CLASS_STUDENTS'),
  MAX_CLASS_TEACHERS: constInt(rosterH, 'ROSTER_MAX_CLASS_TEACHERS'),
  // The per-class face-verify countdown the builder now authors (contract §3.3).
  // roster.h owns these three; the device clamps to them on load.
  FACE_VERIFY_SECONDS_MIN: constInt(rosterH, 'FACE_VERIFY_SECONDS_MIN'),
  FACE_VERIFY_SECONDS_MAX: constInt(rosterH, 'FACE_VERIFY_SECONDS_MAX'),
  FACE_VERIFY_SECONDS_DEFAULT: constInt(rosterH, 'FACE_VERIFY_SECONDS_DEFAULT'),
};

// A class can't reference more professors than the device can hold, so the two
// caps must stay equal (roster.h says so; assert it rather than trusting a comment).
test('ROSTER_MAX_CLASS_TEACHERS equals CONFIG_MAX_TEACHERS', () => {
  assert.equal(constInt(rosterH, 'ROSTER_MAX_CLASS_TEACHERS'),
    constInt(configH, 'CONFIG_MAX_TEACHERS'),
    'a class may reference every teacher, so the caps must match');
});

for (const [key, buf] of Object.entries(LENGTHS)) {
  test(`LIMITS.${key} matches firmware buffer (${buf} - 1)`, () => {
    assert.equal(LIMITS[key], buf - 1,
      `LIMITS.${key}=${LIMITS[key]} but firmware buffer is ${buf} (expected ${buf - 1}). ` +
      'Firmware changed — update validate.js and docs/software/CONFIG_IMPORT.md.');
  });
}

for (const [key, cap] of Object.entries(COUNTS)) {
  test(`LIMITS.${key} matches firmware cap (${cap})`, () => {
    assert.equal(LIMITS[key], cap,
      `LIMITS.${key}=${LIMITS[key]} but firmware cap is ${cap}. Update validate.js and the contract.`);
  });
}

test('every firmware-derived LIMIT is covered by this drift guard', () => {
  // Tool-only limits, with no firmware constant to drift against:
  //   ROSTER_TURMA        — the device ignores the roster-entry turma tag.
  //   MIN_ATTENDANCE_MIN  — the device clamps below 1 with a bare literal.
  //   MIN_ATTENDANCE_MAX  — [builder-stricter]; the device has no upper bound.
  //   MIN_ATTENDANCE_DEFAULT — a literal default in roster_service.cpp.
  // If any of these ever gets a named constant in a header, move it into COUNTS.
  const toolOnly = ['ROSTER_TURMA', 'MIN_ATTENDANCE_MIN', 'MIN_ATTENDANCE_MAX',
    'MIN_ATTENDANCE_DEFAULT'];
  const covered = new Set([...Object.keys(LENGTHS), ...Object.keys(COUNTS), ...toolOnly]);
  const uncovered = Object.keys(LIMITS).filter((k) => !covered.has(k));
  assert.deepEqual(uncovered, [], `LIMITS has keys not checked against firmware: ${uncovered}`);
});

test('the contract doc restates every firmware limit number', {
  skip: contract ? false : 'docs/software/CONFIG_IMPORT.md not found (docs/ restructure) — repoint the path to re-enable',
}, () => {
  // Coarse drift alarm: each canonical number must appear somewhere in
  // docs/software/CONFIG_IMPORT.md. Catches a header change that skipped the contract.
  const missing = [];
  for (const [key, buf] of Object.entries(LENGTHS)) {
    if (!contract.includes(String(buf - 1))) missing.push(`${key} (≤${buf - 1})`);
  }
  for (const [key, cap] of Object.entries(COUNTS)) {
    if (!contract.includes(String(cap))) missing.push(`${key} (${cap})`);
  }
  assert.deepEqual(missing, [], `docs/software/CONFIG_IMPORT.md is missing limit numbers: ${missing.join(', ')}`);
});
