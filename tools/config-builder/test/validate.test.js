import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { validate, LIMITS } from '../src/validate.js';
import { buildClass } from '../src/model.js';

const FIXTURE = JSON.parse(
  readFileSync(fileURLToPath(new URL('../fixtures/example.model.json', import.meta.url)), 'utf8'),
);

// Deep clone so each mutation test starts from the known-good fixture.
const clone = (m) => JSON.parse(JSON.stringify(m));
const has = (errs, part) => errs.some((e) => e.message.includes(part));
const inScope = (errs, scope, field) => errs.some((e) => e.scope === scope && e.field === field);

test('the fixture is valid (no errors)', () => {
  assert.deepEqual(validate(FIXTURE), []);
});

// --- teachers -------------------------------------------------------------

test('no teachers is an error', () => {
  const m = clone(FIXTURE); m.teachers = [];
  assert.ok(has(validate(m), 'lists no teachers'));
});

test('more than 8 teachers is flagged', () => {
  const m = clone(FIXTURE);
  m.teachers = Array.from({ length: 9 }, (_, i) => ({
    name: `T${i}`, email: `t${i}@x.edu`, password: String(i),
  }));
  assert.ok(has(validate(m), `more than ${LIMITS.MAX_TEACHERS} teachers`));
});

test('non-numeric password is rejected with the firmware wording', () => {
  const m = clone(FIXTURE); m.teachers[0].password = '12ab';
  assert.ok(has(validate(m), 'non-numeric password (digits only)'));
});

test('duplicate password is rejected', () => {
  const m = clone(FIXTURE);
  m.teachers[0].password = '9999'; m.teachers[1].password = '9999';
  assert.ok(has(validate(m), 'share the same password'));
});

test('two empty passwords are fine (card-login teachers)', () => {
  const m = clone(FIXTURE);
  m.teachers[0].password = ''; m.teachers[1].password = '';
  // both still have cards, so no "can't log in" error either
  assert.ok(!has(validate(m), 'share the same password'));
  assert.ok(!has(validate(m), "can't log in"));
});

test('duplicate teacher email is rejected', () => {
  const m = clone(FIXTURE); m.teachers[1].email = m.teachers[0].email;
  assert.ok(has(validate(m), 'duplicate teacher email'));
});

test('a teacher with neither card nor password cannot log in', () => {
  const m = clone(FIXTURE);
  m.teachers[0].rfid_uid = ''; m.teachers[0].password = '';
  assert.ok(has(validate(m), "can't log in"));
});

test('over-length teacher name is rejected', () => {
  const m = clone(FIXTURE); m.teachers[0].name = 'x'.repeat(LIMITS.TEACHER_NAME + 1);
  assert.ok(inScope(validate(m), 'teacher', 'name'));
});

// --- students -------------------------------------------------------------

test('duplicate student id is rejected', () => {
  const m = clone(FIXTURE); m.students[1].id = m.students[0].id;
  assert.ok(has(validate(m), `duplicate id ${m.students[0].id}`));
});

test('missing student name is rejected', () => {
  const m = clone(FIXTURE); m.students[0].name = '';
  assert.ok(inScope(validate(m), 'student', 'name'));
});

test('shared student UID (different formatting) is caught after normalization', () => {
  const m = clone(FIXTURE);
  m.students[0].rfid_uid = 'AA:BB:CC:DD';
  m.students[1].rfid_uid = 'aabbccdd';
  assert.ok(has(validate(m), 'share RFID uid'));
});

test('student UID colliding with a teacher card is rejected', () => {
  const m = clone(FIXTURE);
  m.students[0].rfid_uid = m.teachers[0].rfid_uid; // "E0:D1:33:5F"
  assert.ok(has(validate(m), 'shares RFID uid') || has(validate(m), 'with a teacher'));
});

test('more than the student cap is flagged', () => {
  const m = clone(FIXTURE);
  m.students = Array.from({ length: LIMITS.MAX_STUDENTS + 1 }, (_, i) => ({
    id: `id-${i}`, name: `S${i}`,
  }));
  assert.ok(has(validate(m), `more than ${LIMITS.MAX_STUDENTS} students`));
});

test('over-length student id is rejected', () => {
  const m = clone(FIXTURE); m.students[0].id = '1'.repeat(LIMITS.STUDENT_ID + 1);
  assert.ok(inScope(validate(m), 'student', 'id'));
});

// --- classes --------------------------------------------------------------

test('roster referencing an unknown student id is rejected', () => {
  const m = clone(FIXTURE); m.classes[0].roster.push('9999-9999');
  assert.ok(has(validate(m), 'unknown student 9999-9999 in roster'));
});

test('duplicate class code is rejected', () => {
  const m = clone(FIXTURE); m.classes[1].code = m.classes[0].code;
  assert.ok(has(validate(m), 'duplicate class code'));
});

test('non-hex color is rejected', () => {
  const m = clone(FIXTURE); m.classes[0].color = 'ZZZ';
  assert.ok(has(validate(m), '6 hex digits'));
});

test('leading-hash color is rejected (contract wants bare 6-hex)', () => {
  const m = clone(FIXTURE); m.classes[0].color = '#272766';
  assert.ok(has(validate(m), '6 hex digits'));
});

test('teacher_email matching no teacher is rejected', () => {
  const m = clone(FIXTURE); m.classes[0].teacher_emails = ['ghost@nowhere.edu'];
  assert.ok(has(validate(m), 'matches no teacher'));
});

// --- multi-professor classes (CONFIG_IMPORT.md §3.3 teacher_emails) ---------

test('a class co-taught by several professors is valid', () => {
  const m = clone(FIXTURE);
  m.teachers.push({ name: 'Second', email: 'second@x.edu', rfid_uid: '', password: '9911' });
  m.classes[0].teacher_emails = [m.teachers[0].email, 'second@x.edu'];
  assert.deepEqual(validate(m), []);
});

test('a class with no professor selected is rejected', () => {
  const m = clone(FIXTURE); m.classes[0].teacher_emails = [];
  assert.ok(has(validate(m), 'has no professor selected'));
});

test('the same professor listed twice is de-duplicated, not an error', () => {
  const m = clone(FIXTURE);
  const e = m.teachers[0].email;
  m.classes[0].teacher_emails = [e, e];
  assert.deepEqual(validate(m), []);
  // The tar carries one entry, so the device never sees the duplicate.
  assert.deepEqual(buildClass(m.classes[0]).teacher_emails, [e]);
});

test('more professors than the firmware cap is rejected', () => {
  const m = clone(FIXTURE);
  m.classes[0].teacher_emails = [];
  for (let i = 0; i < LIMITS.MAX_CLASS_TEACHERS + 1; i++) {
    const email = `p${i}@x.edu`;
    m.teachers.push({ name: `P${i}`, email, rfid_uid: '', password: String(70000 + i) });
    m.classes[0].teacher_emails.push(email);
  }
  assert.ok(has(validate(m), `more than ${LIMITS.MAX_CLASS_TEACHERS} professors`));
});

test('a legacy scalar teacher_email is still accepted', () => {
  const m = clone(FIXTURE);
  delete m.classes[0].teacher_emails;
  m.classes[0].teacher_email = m.teachers[0].email;
  assert.deepEqual(validate(m), []);
});

test('class code with a path separator is rejected (zip-slip defense)', () => {
  const m = clone(FIXTURE); m.classes[0].code = '../evil';
  assert.ok(has(validate(m), 'not a valid folder name'));
});

test('duplicate roster id within a class is rejected', () => {
  const m = clone(FIXTURE);
  m.classes[0].roster = [m.students[0].id, m.students[0].id];
  assert.ok(has(validate(m), 'listed twice'));
});

test('the same student cannot be in one class with two different turmas', () => {
  const m = clone(FIXTURE);
  m.classes[0].roster = [
    { id: m.students[0].id, turma: 'TE1' },
    { id: m.students[0].id, turma: 'TE2' },
  ];
  const errs = validate(m);
  assert.ok(has(errs, 'listed twice with different turmas'));
  assert.ok(has(errs, 'one turma per student per class'));
});

test('the same student may be in different classes with different turmas', () => {
  const m = clone(FIXTURE);
  // students[0] is already in class 0 (turma M1); also enroll in class 1 with a
  // different turma. Different classes → allowed.
  m.classes[1].roster.push({ id: m.students[0].id, turma: 'ZZ' });
  assert.deepEqual(validate(m), []);
});

test('more than 12 classes is flagged', () => {
  const m = clone(FIXTURE);
  m.classes = Array.from({ length: LIMITS.MAX_CLASSES + 1 }, (_, i) => ({
    code: `C${i}`, name: `Class ${i}`, teacher_emails: [m.teachers[0].email], color: '272766', roster: [],
  }));
  assert.ok(has(validate(m), `more than ${LIMITS.MAX_CLASSES} classes`));
});

test('over-length roster (> MAX_CLASS_ROSTER) is flagged', () => {
  const m = clone(FIXTURE);
  m.students = Array.from({ length: LIMITS.MAX_CLASS_ROSTER + 1 }, (_, i) => ({ id: `s-${i}`, name: `S${i}` }));
  m.classes = [{
    code: 'BIG', name: 'Big', teacher_emails: [m.teachers[0].email], color: '272766',
    roster: m.students.map((s) => s.id),
  }];
  assert.ok(has(validate(m), `more than ${LIMITS.MAX_CLASS_ROSTER} students in roster`));
});

test('a class is unblockable when no teacher has an email — the error says so', () => {
  const m = clone(FIXTURE);
  // Teachers exist but none has an email: the class cannot be linked to any of
  // them (the link is by email), so the message must point at the real fix.
  m.teachers.forEach((t) => { t.email = ''; });
  m.classes.forEach((c) => { c.teacher_emails = []; });
  const errs = validate(m);
  assert.ok(has(errs, 'give a teacher an email first'),
    'the class error must name the blocking problem, not just restate the symptom');
});

test('with a selectable teacher the class error stays short', () => {
  const m = clone(FIXTURE);
  m.classes[0].teacher_emails = [];
  const errs = validate(m);
  assert.ok(has(errs, 'has no professor selected'));
  assert.ok(!has(errs, 'give a teacher an email first'));
});
