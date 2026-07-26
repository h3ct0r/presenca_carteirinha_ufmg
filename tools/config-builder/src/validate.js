// Pure, DOM-free validation of the authoring model against the contract
// (docs/CONFIG_IMPORT.md §3). Returns a flat list of structured errors so the
// UI can point at the offending row and the export button can gate on
// `errors.length === 0`.
//
// Error shape: { scope, index, field, message }
//   scope   'config' | 'teacher' | 'student' | 'class' | 'roster'
//   index   0-based row index within that scope (or -1 for whole-file issues)
//   field   the offending field name (or '' )
//   message human-readable; mirrors the firmware's wording where practical.
//
// Where this file is deliberately STRICTER than the device's load validator,
// it is following the contract, which is stricter on purpose so authoring
// mistakes fail on the laptop instead of silently degrading on-device:
//   - string length limits (device merely truncates; contract §3 says reject);
//   - a student UID may not equal any teacher UID (contract §3.2 — the device
//     enforces this at enroll time, not at load);
//   - teacher_email must reference a defined teacher (device only warns);
//   - a teacher must be reachable (a card or a password), else it can't log in.
// Each such rule is commented [builder-stricter].

import { uidNormalize } from './uid.js';

// Limits mirror the firmware buffer sizes (max chars = buffer − 1) and caps.
export const LIMITS = {
  MAX_TEACHERS: 8,
  MAX_STUDENTS: 300,
  MAX_CLASSES: 12,
  MAX_CLASS_ROSTER: 100,
  TEACHER_NAME: 47,
  TEACHER_EMAIL: 63,
  TEACHER_UID: 39,
  TEACHER_PASSWORD: 31,
  STUDENT_ID: 19,
  STUDENT_NAME: 47,
  STUDENT_UID: 23,
  CLASS_CODE: 23,
  CLASS_NAME: 47,
  CLASS_SCHEDULE: 39,
  CLASS_TEACHER_EMAIL: 63,
  ROSTER_TURMA: 15,
};

const HEX6 = /^[0-9a-fA-F]{6}$/;
const DIGITS = /^[0-9]+$/;

export function validate(model) {
  const errors = [];
  const add = (scope, index, field, message) => errors.push({ scope, index, field, message });

  const teachers = Array.isArray(model.teachers) ? model.teachers : [];
  const students = Array.isArray(model.students) ? model.students : [];
  const classes = Array.isArray(model.classes) ? model.classes : [];

  validateTeachers(teachers, add);
  validateStudents(students, teachers, add);
  validateClasses(classes, students, teachers, add);

  return errors;
}

function str(v) {
  return v == null ? '' : String(v);
}

function validateTeachers(teachers, add) {
  if (teachers.length === 0) {
    add('config', -1, 'teachers', 'config.json lists no teachers');
  }
  if (teachers.length > LIMITS.MAX_TEACHERS) {
    // [builder-stricter] the device silently ignores teachers past the 8th;
    // surface it so none are dropped unnoticed.
    add('config', -1, 'teachers',
      `more than ${LIMITS.MAX_TEACHERS} teachers (the device honors only the first ${LIMITS.MAX_TEACHERS})`);
  }

  const emailSeen = new Map();
  const pwSeen = new Map();
  const uidSeen = new Map();

  teachers.forEach((t, i) => {
    const name = str(t.name);
    const email = str(t.email);
    const uid = str(t.rfid_uid);
    const pw = str(t.password);
    const who = name || email || `teacher ${i + 1}`;

    if (!name) add('teacher', i, 'name', `teacher ${i + 1} has no name`);
    if (name.length > LIMITS.TEACHER_NAME)
      add('teacher', i, 'name', `${who}: name exceeds ${LIMITS.TEACHER_NAME} characters`);

    // [builder-stricter] classes link to a teacher by email; empty makes the
    // class unassignable, and duplicates make the link ambiguous.
    if (!email) add('teacher', i, 'email', `${who} has no email`);
    if (email.length > LIMITS.TEACHER_EMAIL)
      add('teacher', i, 'email', `${who}: email exceeds ${LIMITS.TEACHER_EMAIL} characters`);
    if (email) {
      const prev = emailSeen.get(email);
      if (prev !== undefined) add('teacher', i, 'email', `duplicate teacher email ${email}`);
      else emailSeen.set(email, i);
    }

    if (uid.length > LIMITS.TEACHER_UID)
      add('teacher', i, 'rfid_uid', `${who}: rfid_uid exceeds ${LIMITS.TEACHER_UID} characters`);
    if (uid) {
      const n = uidNormalize(uid);
      const prev = uidSeen.get(n);
      if (prev !== undefined) add('teacher', i, 'rfid_uid', `${who} shares RFID uid ${uid} with another teacher`);
      else uidSeen.set(n, i);
    }

    if (pw) {
      if (!DIGITS.test(pw))
        add('teacher', i, 'password', `${who} has a non-numeric password (digits only)`);
      if (pw.length > LIMITS.TEACHER_PASSWORD)
        add('teacher', i, 'password', `${who}: password exceeds ${LIMITS.TEACHER_PASSWORD} digits`);
      const prev = pwSeen.get(pw);
      if (prev !== undefined) {
        const other = str(teachers[prev].name) || `teacher ${prev + 1}`;
        add('teacher', i, 'password', `${other} and ${who} share the same password`);
      } else {
        pwSeen.set(pw, i);
      }
    }

    // [builder-stricter] a teacher with neither a card nor a password can
    // never authenticate on the device.
    if (!uid && !pw)
      add('teacher', i, 'rfid_uid', `${who} has neither a card nor a password — they can't log in`);
  });
}

function validateStudents(students, teachers, add) {
  if (students.length > LIMITS.MAX_STUDENTS)
    add('config', -1, 'students', `more than ${LIMITS.MAX_STUDENTS} students`);

  // Teacher UIDs, normalized, for the cross-holder collision check.
  const teacherUids = new Map();
  teachers.forEach((t, i) => {
    const uid = str(t.rfid_uid);
    if (uid) teacherUids.set(uidNormalize(uid), i);
  });

  const idSeen = new Map();
  const uidSeen = new Map();

  students.forEach((s, i) => {
    const id = str(s.id);
    const name = str(s.name);
    const uid = s.rfid_uid == null ? '' : String(s.rfid_uid);

    if (!id) add('student', i, 'id', `student ${i + 1} has no id`);
    if (id.length > LIMITS.STUDENT_ID)
      add('student', i, 'id', `student ${id || i + 1}: id exceeds ${LIMITS.STUDENT_ID} characters`);
    if (!name) add('student', i, 'name', `student ${id || i + 1} has no name`);
    if (name.length > LIMITS.STUDENT_NAME)
      add('student', i, 'name', `student ${id || i + 1}: name exceeds ${LIMITS.STUDENT_NAME} characters`);

    if (id) {
      if (idSeen.has(id)) add('student', i, 'id', `duplicate id ${id}`);
      else idSeen.set(id, i);
    }

    if (uid) {
      if (uid.length > LIMITS.STUDENT_UID)
        add('student', i, 'rfid_uid', `student ${id || i + 1}: rfid_uid exceeds ${LIMITS.STUDENT_UID} characters`);
      const n = uidNormalize(uid);
      if (uidSeen.has(n)) {
        const other = str(students[uidSeen.get(n)].id);
        add('student', i, 'rfid_uid', `${other} and ${id} share RFID uid ${uid}`);
      } else {
        uidSeen.set(n, i);
      }
      // [builder-stricter, per contract §3.2] a card belongs to one holder.
      if (teacherUids.has(n))
        add('student', i, 'rfid_uid', `student ${id} shares RFID uid ${uid} with a teacher`);
    }
  });
}

function validateClasses(classes, students, teachers, add) {
  if (classes.length > LIMITS.MAX_CLASSES)
    add('config', -1, 'classes', `more than ${LIMITS.MAX_CLASSES} classes`);

  const studentIds = new Set(students.map((s) => str(s.id)).filter(Boolean));
  const teacherEmails = new Set(teachers.map((t) => str(t.email)).filter(Boolean));
  const codeSeen = new Map();

  classes.forEach((c, i) => {
    const code = str(c.code);
    const name = str(c.name);
    const schedule = str(c.schedule);
    const email = str(c.teacher_email);
    const color = str(c.color);
    const who = code || `class ${i + 1}`;

    if (!code) add('class', i, 'code', `class ${i + 1} has no code`);
    if (code.length > LIMITS.CLASS_CODE)
      add('class', i, 'code', `${who}: code exceeds ${LIMITS.CLASS_CODE} characters`);
    // The code becomes a folder name (classes/<code>/) — it must be a safe path
    // segment (contract §4 whitelist: no '/', no '..').
    if (code && (code.includes('/') || code.includes('\\') || code.includes('..') || code === '.'))
      add('class', i, 'code', `${who}: code is not a valid folder name (no '/', '\\' or '..')`);
    if (code) {
      if (codeSeen.has(code)) add('class', i, 'code', `duplicate class code ${code}`);
      else codeSeen.set(code, i);
    }

    if (!name) add('class', i, 'name', `${who} has no name`);
    if (name.length > LIMITS.CLASS_NAME)
      add('class', i, 'name', `${who}: name exceeds ${LIMITS.CLASS_NAME} characters`);
    if (schedule.length > LIMITS.CLASS_SCHEDULE)
      add('class', i, 'schedule', `${who}: schedule exceeds ${LIMITS.CLASS_SCHEDULE} characters`);

    // [builder-stricter] the device only warns, but an unassigned or dangling
    // teacher_email means the class shows under nobody.
    if (!email) add('class', i, 'teacher_email', `${who} has no teacher_email`);
    else if (email.length > LIMITS.CLASS_TEACHER_EMAIL)
      add('class', i, 'teacher_email', `${who}: teacher_email exceeds ${LIMITS.CLASS_TEACHER_EMAIL} characters`);
    else if (!teacherEmails.has(email))
      add('class', i, 'teacher_email', `${who}: teacher_email ${email} matches no teacher`);

    if (color && !HEX6.test(color))
      add('class', i, 'color', `${who}: color must be 6 hex digits (no leading '#')`);

    const roster = Array.isArray(c.roster) ? c.roster : [];
    if (roster.length > LIMITS.MAX_CLASS_ROSTER)
      add('class', i, 'roster', `${who}: more than ${LIMITS.MAX_CLASS_ROSTER} students in roster`);

    // A student appears at most once per class — one turma per student per class.
    // Track the first turma seen so a repeat with a *different* turma gets a
    // pointed message (the exact "same student, two turmas in one class" case).
    const seen = new Map(); // sid -> first turma seen
    roster.forEach((entry) => {
      // Roster entries are { id, turma? }; a bare id string is tolerated.
      const sid = typeof entry === 'string' ? entry : str(entry && entry.id);
      const turma = typeof entry === 'string' ? '' : str(entry && entry.turma);
      if (!sid) {
        add('class', i, 'roster', `${who}: a roster entry has no id`);
        return;
      }
      if (!studentIds.has(sid)) add('class', i, 'roster', `${who}: unknown student ${sid} in roster`);
      if (seen.has(sid)) {
        const prev = seen.get(sid);
        if (prev !== turma)
          add('class', i, 'roster',
            `${who}: student ${sid} listed twice with different turmas ('${prev || '—'}' vs '${turma || '—'}') — one turma per student per class`);
        else
          add('class', i, 'roster', `${who}: student ${sid} listed twice`);
      } else {
        seen.set(sid, turma);
      }
      if (turma.length > LIMITS.ROSTER_TURMA)
        add('class', i, 'roster', `${who}: turma for ${sid} exceeds ${LIMITS.ROSTER_TURMA} characters`);
    });
  });
}
