// Pure, DOM-free translation of the authoring model into the three on-card
// JSON documents defined by docs/software/CONFIG_IMPORT.md §3.
//
// The "authoring model" is the shape the UI (and fixtures/example.model.json)
// works with — flat and convenient to edit. This module turns it into the exact
// files the device reads. Validation lives in validate.js; buildFiles assumes a
// model that already passed it (it only normalizes shapes, it does not reject).
//
// Authoring model:
//   {
//     teachers: [ { name, email, rfid_uid?, password? } ],
//     students: [ { id, name, rfid_uid? } ],      // rfid_uid "" / undefined => null
//     classes:  [ { code, name, schedule?, teacher_emails?: [email,...], color?,
//                   roster: [id,...] } ]   // legacy scalar teacher_email accepted
//   }

const DEFAULT_COLOR = '272766';

// Contract §3.3 defaults for the per-class attendance settings. They match the
// firmware's own fallbacks (roster.h / roster_service.cpp), so a class that
// never touched these settings emits exactly what the device would have used.
export const DEFAULT_FACE_VERIFY_SECONDS = 15;
export const DEFAULT_MIN_ATTENDANCE_MIN = 45;

// A whole number, or `fallback` when the value is blank/absent/not a number.
// Out-of-range values pass through untouched — validate.js rejects them, so a
// typo surfaces as an error instead of being silently rewritten.
function intOr(value, fallback) {
  if (value === undefined || value === null || value === '') return fallback;
  const n = Number(value);
  return Number.isFinite(n) ? Math.trunc(n) : fallback;
}

// Palette for auto-assigned class colours. The device paints this behind the
// class initial in WHITE (`scr_classes.cpp` draws the 40x40 chip with
// THEME_ON_PRIMARY text), so a light colour would be unreadable — every entry
// here clears WCAG AA (>= 4.5:1 contrast against white). Hues are spread around
// the wheel so classes stay distinguishable at a glance, and there are more
// entries (14) than the 12-class cap, so every class can get a unique one.
export const CLASS_COLORS = [
  '272766', // indigo (the historical default)
  'C62828', // red
  'AD1457', // pink
  '6A1B9A', // purple
  '4527A0', // deep purple
  '1565C0', // blue
  '0277BD', // light blue
  '00838F', // cyan
  '00695C', // teal
  '2E7D32', // green
  '827717', // olive
  '5D4037', // brown
  '455A64', // blue grey
  'B71C1C', // dark red
];

// Picks a colour for a new class: random, but never one already in `used`, so
// two classes don't come out looking the same. Falls back to a random palette
// entry once every colour is taken. `rng` is injectable so tests are
// deterministic (defaults to Math.random).
export function pickClassColor(used = [], rng = Math.random) {
  const taken = new Set(
    [].concat(used).filter(Boolean).map((c) => String(c).replace('#', '').toUpperCase()),
  );
  const free = CLASS_COLORS.filter((c) => !taken.has(c));
  const pool = free.length ? free : CLASS_COLORS;
  return pool[Math.floor(rng() * pool.length) % pool.length];
}

// The professors of a class, normalized to a de-duplicated string array.
// A class may be co-taught (CONFIG_IMPORT.md §3.3 "teacher_emails"). Accepts the
// legacy scalar `teacher_email` too, so older model JSON keeps loading.
export function classTeacherEmails(cls) {
  const raw = Array.isArray(cls?.teacher_emails)
    ? cls.teacher_emails
    : (cls?.teacher_email ? [cls.teacher_email] : []);
  const out = [];
  for (const e of raw) {
    const s = String(e ?? '').trim();
    if (s && !out.includes(s)) out.push(s);
  }
  return out;
}

export function buildConfig(model) {
  return {
    teachers: (model.teachers || []).map((t) => ({
      name: t.name ?? '',
      email: t.email ?? '',
      rfid_uid: t.rfid_uid ? t.rfid_uid : '',
      password: t.password ? String(t.password) : '',
    })),
  };
}

export function buildStudents(model) {
  return {
    version: 1,
    students: (model.students || []).map((s) => ({
      id: s.id ?? '',
      name: s.name ?? '',
      rfid_uid: s.rfid_uid ? s.rfid_uid : null,
    })),
  };
}

export function buildClass(cls) {
  return {
    version: 1,
    code: cls.code ?? '',
    name: cls.name ?? '',
    schedule: cls.schedule ?? '',
    // Always the array form — the device reads the legacy scalar, but the
    // builder emits only the current contract (CONFIG_IMPORT.md §3.3).
    teacher_emails: classTeacherEmails(cls),
    color: cls.color ? cls.color : DEFAULT_COLOR,
    // Per-class attendance settings (CONFIG_IMPORT.md §3.3). Always emitted, so
    // a class states its check-in behaviour instead of inheriting whatever the
    // device happened to have — an import overwrites these like any other
    // authored field. The two booleans are independent on the device; this tool
    // authors one mode at a time (see checkinMode() in validate.js).
    capture_photos: !!cls.capture_photos,
    face_verify_seconds: intOr(cls.face_verify_seconds, DEFAULT_FACE_VERIFY_SECONDS),
    timed_attendance: !!cls.timed_attendance,
    min_attendance_min: intOr(cls.min_attendance_min, DEFAULT_MIN_ATTENDANCE_MIN),
    // Roster entries are {id, turma?}. A bare id string is tolerated (turma "").
    // turma is the optional per-student class-group tag (see CONFIG_IMPORT §3.3).
    roster: (cls.roster || []).map((r) => {
      const id = typeof r === 'string' ? r : (r.id ?? '');
      const turma = typeof r === 'string' ? '' : (r.turma ?? '');
      return turma ? { id, turma } : { id };
    }),
  };
}

// Returns [{ name, data }] ready for makeTar(). Paths are exactly the three
// authored kinds from the contract's whitelist (§4) — nothing else.
export function buildFiles(model) {
  const files = [
    { name: 'config.json', data: json(buildConfig(model)) },
    { name: 'students/students.json', data: json(buildStudents(model)) },
  ];
  for (const cls of model.classes || []) {
    files.push({
      name: `classes/${cls.code}/class.json`,
      data: json(buildClass(cls)),
    });
  }
  return files;
}

function json(obj) {
  return JSON.stringify(obj, null, 2) + '\n';
}
