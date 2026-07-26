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
//     capture_photos: bool,
//     teachers: [ { name, email, rfid_uid?, password? } ],
//     students: [ { id, name, rfid_uid? } ],      // rfid_uid "" / undefined => null
//     classes:  [ { code, name, schedule?, teacher_email?, color?, roster: [id,...] } ]
//   }

const DEFAULT_COLOR = '272766';

export function buildConfig(model) {
  return {
    capture_photos: !!model.capture_photos,
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
    teacher_email: cls.teacher_email ?? '',
    color: cls.color ? cls.color : DEFAULT_COLOR,
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
