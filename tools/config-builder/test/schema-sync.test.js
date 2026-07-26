import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { LIMITS } from '../src/validate.js';

// Schema drift guard. The firmware is the source of truth for the on-card
// format (docs/CONFIG_IMPORT.md restates it; this tool validates against it).
// This test parses the firmware headers and asserts the builder's LIMITS still
// match — so if someone changes a buffer size or a cap on the device and forgets
// the tool/contract, `node --test` goes red. See tools/config-builder/CLAUDE.md
// ("Keeping in sync") for the required change order.

const root = (p) => readFileSync(fileURLToPath(new URL('../../../' + p, import.meta.url)), 'utf8');

const rosterH = root('include/app/roster.h');
const teacherH = root('include/app/teacher.h');
const configH = root('include/services/config_service.h');
const contract = root('docs/CONFIG_IMPORT.md');

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
  CLASS_TEACHER_EMAIL: charBuf(klass, 'teacher_email'),
};

// LIMIT key  ->  firmware cap constant (equal).
const COUNTS = {
  MAX_TEACHERS: constInt(configH, 'CONFIG_MAX_TEACHERS'),
  MAX_STUDENTS: constInt(rosterH, 'ROSTER_MAX_STUDENTS'),
  MAX_CLASSES: constInt(rosterH, 'ROSTER_MAX_CLASSES'),
  MAX_CLASS_ROSTER: constInt(rosterH, 'ROSTER_MAX_CLASS_STUDENTS'),
};

for (const [key, buf] of Object.entries(LENGTHS)) {
  test(`LIMITS.${key} matches firmware buffer (${buf} - 1)`, () => {
    assert.equal(LIMITS[key], buf - 1,
      `LIMITS.${key}=${LIMITS[key]} but firmware buffer is ${buf} (expected ${buf - 1}). ` +
      'Firmware changed — update validate.js and docs/CONFIG_IMPORT.md.');
  });
}

for (const [key, cap] of Object.entries(COUNTS)) {
  test(`LIMITS.${key} matches firmware cap (${cap})`, () => {
    assert.equal(LIMITS[key], cap,
      `LIMITS.${key}=${LIMITS[key]} but firmware cap is ${cap}. Update validate.js and the contract.`);
  });
}

test('every firmware-derived LIMIT is covered by this drift guard', () => {
  // ROSTER_TURMA has no firmware buffer (the device ignores the roster-entry
  // turma tag), so it is intentionally tool-only and excluded here.
  const covered = new Set([...Object.keys(LENGTHS), ...Object.keys(COUNTS), 'ROSTER_TURMA']);
  const uncovered = Object.keys(LIMITS).filter((k) => !covered.has(k));
  assert.deepEqual(uncovered, [], `LIMITS has keys not checked against firmware: ${uncovered}`);
});

test('the contract doc restates every firmware limit number', () => {
  // Coarse drift alarm: each canonical number must appear somewhere in
  // docs/CONFIG_IMPORT.md. Catches a header change that skipped the contract.
  const missing = [];
  for (const [key, buf] of Object.entries(LENGTHS)) {
    if (!contract.includes(String(buf - 1))) missing.push(`${key} (≤${buf - 1})`);
  }
  for (const [key, cap] of Object.entries(COUNTS)) {
    if (!contract.includes(String(cap))) missing.push(`${key} (${cap})`);
  }
  assert.deepEqual(missing, [], `docs/CONFIG_IMPORT.md is missing limit numbers: ${missing.join(', ')}`);
});
