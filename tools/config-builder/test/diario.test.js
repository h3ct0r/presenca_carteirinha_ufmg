import { test } from 'node:test';
import assert from 'node:assert/strict';
import { parseDiario, decodeCsvBytes, applyDiario } from '../src/diario.js';
import { validate } from '../src/validate.js';
import { buildFiles, CLASS_COLORS, pickClassColor } from '../src/model.js';

// The exact sample the user provided (UTF-8 here in source).
const SAMPLE = `PERIODO;ATIVIDADE;TURMA
2026/2;DCC219;TE1

N.;MATRICULA;NOME;PTS OBTIDOS
1;2025115525;ALIGIA CASSIO DOS SANTOS;0,00
2;2025049999;ALINE CRISTINA GONCALVES DA COSTA;0,00
3;2022069239;ALINE REGINA DEODORO;0,00
4;2025050768;ANA LAURA MACHADO MOTA;0,00
5;2026431730;ANA LUISA PESSOA COSTA;0,00
6;2026046241;ARTHUR GUILHERME SANTOS DA SILVA;0,00
7;2025087440;ARTHUR RESENDE DE FREITAS ROCHA;0,00
8;2025422517;BARBARA PAGNOCCA ANDRADE;0,00
9;2018107393;CAIO OTÁVIO DE SOUZA MESSIAS;0,00
10;2026047132;DANILO ROCHA ALVARENGA NOGUEIRA AMORIM;0,00
`;

test('parses activity, turma and period from the header block', () => {
  const d = parseDiario(SAMPLE);
  assert.equal(d.atividade, 'DCC219');
  assert.equal(d.turma, 'TE1');
  assert.equal(d.periodo, '2026/2');
});

test('extracts only matricula + nome, skipping N. and PTS', () => {
  const d = parseDiario(SAMPLE);
  assert.equal(d.students.length, 10);
  assert.deepEqual(d.students[0], { id: '2025115525', name: 'ALIGIA CASSIO DOS SANTOS' });
});

test('preserves Portuguese accents in names', () => {
  const d = parseDiario(SAMPLE);
  const caio = d.students.find((s) => s.id === '2018107393');
  assert.equal(caio.name, 'CAIO OTÁVIO DE SOUZA MESSIAS');
});

test('the blank separator line is ignored', () => {
  const d = parseDiario(SAMPLE);
  assert.ok(d.students.every((s) => s.id && s.name));
});

test('throws on a file without a MATRICULA/NOME header', () => {
  assert.throws(() => parseDiario('a;b;c\n1;2;3\n'), /Diário/);
});

test('deduplicates repeated matriculas', () => {
  const dup = SAMPLE + '11;2025115525;ALIGIA CASSIO DOS SANTOS;0,00\n';
  const d = parseDiario(dup);
  assert.equal(d.students.length, 10);
  assert.equal(d.duplicates, 1);
});

// --- encoding -------------------------------------------------------------
test('decodes UTF-8 bytes (with accents)', () => {
  const bytes = new TextEncoder().encode('N.;MATRICULA;NOME\n1;123;OTÁVIO\n');
  const d = parseDiario(decodeCsvBytes(bytes));
  assert.equal(d.students[0].name, 'OTÁVIO');
});

test('falls back to windows-1252 when bytes are not valid UTF-8', () => {
  // "OTÁVIO" with Á as the Latin-1 byte 0xC1 (invalid as a lone UTF-8 byte).
  const head = [...'N.;MATRICULA;NOME\n1;123;OT'].map((c) => c.charCodeAt(0));
  const tail = [...'VIO\n'].map((c) => c.charCodeAt(0));
  const bytes = new Uint8Array([...head, 0xc1, ...tail]);
  const text = decodeCsvBytes(bytes);
  const d = parseDiario(text);
  assert.equal(d.students[0].name, 'OTÁVIO');
});

// --- applyDiario ----------------------------------------------------------
test('applyDiario keys the class as <SEMESTER>-<ATIVIDADE> and tags roster entries', () => {
  const model = { teachers: [], students: [], classes: [] };
  const summary = applyDiario(model, parseDiario(SAMPLE));
  assert.equal(summary.code, '2026_2-DCC219');   // semester '/' → '_'
  assert.equal(summary.turma, 'TE1');
  assert.equal(summary.addedStudents, 10);
  assert.equal(model.classes.length, 1);
  assert.equal(model.classes[0].code, '2026_2-DCC219');
  // turma lives on each roster entry, not on the student registry.
  assert.ok(model.classes[0].roster.every((e) => e.turma === 'TE1'));
  assert.ok(model.students.every((s) => !('turma' in s)));
});

test('two turmas of the same course+semester merge into one class, tagged per student', () => {
  const TE2 = `PERIODO;ATIVIDADE;TURMA
2026/2;DCC219;TE2

N.;MATRICULA;NOME;PTS OBTIDOS
1;3000000001;NOVO ALUNO UM;0,00`;
  const model = { teachers: [], students: [], classes: [] };
  applyDiario(model, parseDiario(SAMPLE)); // DCC219 TE1 (10 students)
  applyDiario(model, parseDiario(TE2));    // DCC219 TE2 (1 new student), same key
  assert.equal(model.classes.length, 1);   // merged into 2026_2-DCC219
  const cls = model.classes[0];
  assert.equal(cls.code, '2026_2-DCC219');
  assert.equal(cls.roster.length, 11);
  assert.equal(cls.roster.find((e) => e.id === '2025115525').turma, 'TE1');
  assert.equal(cls.roster.find((e) => e.id === '3000000001').turma, 'TE2');
});

test('import creates the class if absent and enrolls each student with id + turma', () => {
  const model = { teachers: [], students: [], classes: [] };
  assert.equal(model.classes.length, 0);
  applyDiario(model, parseDiario(SAMPLE));
  // class was created, keyed by code
  assert.equal(model.classes.length, 1);
  const cls = model.classes[0];
  assert.equal(cls.code, '2026_2-DCC219');
  // every imported student is a registry record AND a roster entry {id, turma}
  for (const s of parseDiario(SAMPLE).students) {
    assert.ok(model.students.find((r) => r.id === s.id), `student ${s.id} in registry`);
    const entry = cls.roster.find((e) => e.id === s.id);
    assert.ok(entry, `student ${s.id} in class roster`);
    assert.equal(entry.turma, 'TE1');
  }
});

test('import backfills blank class fields (name, color, teacher) without overwriting set ones', () => {
  const model = {
   
    teachers: [{ name: 'Prof', email: 'p@x.edu', rfid_uid: '', password: '1' }],
    students: [],
    // a pre-existing class with the same code but blank fields and a custom name
    classes: [{ code: '2026_2-DCC219', name: 'Kept Name', schedule: '', teacher_emails: [], color: '', roster: [] }],
  };
  applyDiario(model, parseDiario(SAMPLE));
  const cls = model.classes[0];
  assert.equal(model.classes.length, 1);       // matched the existing class, no dup
  assert.equal(cls.name, 'Kept Name');         // author's value preserved
  assert.ok(CLASS_COLORS.includes(cls.color));  // blank → a palette colour filled
  assert.deepEqual(cls.teacher_emails, ['p@x.edu']);  // blank → lone teacher filled
  assert.equal(cls.roster.length, 10);         // students enrolled
});

test('a different course keys a different class; a shared student differs per class', () => {
  const OTHER = `PERIODO;ATIVIDADE;TURMA
2026/2;DCC220;TG1

N.;MATRICULA;NOME;PTS OBTIDOS
1;2025115525;ALIGIA CASSIO DOS SANTOS;0,00`;
  const model = { teachers: [], students: [], classes: [] };
  applyDiario(model, parseDiario(SAMPLE)); // 2026_2-DCC219 (turma TE1)
  applyDiario(model, parseDiario(OTHER));  // 2026_2-DCC220 (turma TG1)
  assert.equal(model.students.length, 10); // one global record for the shared id
  assert.equal(model.classes.length, 2);
  const c219 = model.classes.find((c) => c.code === '2026_2-DCC219');
  const c220 = model.classes.find((c) => c.code === '2026_2-DCC220');
  assert.equal(c219.roster.find((e) => e.id === '2025115525').turma, 'TE1');
  assert.equal(c220.roster.find((e) => e.id === '2025115525').turma, 'TG1');
});

test('re-importing the same Diário updates, does not duplicate', () => {
  const model = { teachers: [], students: [], classes: [] };
  applyDiario(model, parseDiario(SAMPLE));
  const summary = applyDiario(model, parseDiario(SAMPLE));
  assert.equal(model.students.length, 10);
  assert.equal(model.classes.length, 1);
  assert.equal(summary.updatedStudents, 10);
  assert.equal(summary.addedStudents, 0);
});

test('a single defined teacher is auto-assigned to the imported class', () => {
  const model = {
   
    teachers: [{ name: 'Prof', email: 'p@x.edu', rfid_uid: '', password: '1' }],
    students: [], classes: [],
  };
  applyDiario(model, parseDiario(SAMPLE));
  assert.deepEqual(model.classes[0].teacher_emails, ['p@x.edu']);
});

test('imported model (with a teacher assigned) validates and builds turma into the tar files', () => {
  const model = {
   
    teachers: [{ name: 'Prof', email: 'p@x.edu', rfid_uid: '', password: '1' }],
    students: [], classes: [],
  };
  applyDiario(model, parseDiario(SAMPLE));
  assert.deepEqual(validate(model), []);
  const files = buildFiles(model);
  const students = JSON.parse(files.find((f) => f.name === 'students/students.json').data);
  assert.equal(students.students[0].turma, undefined); // no turma on the registry
  const cls = JSON.parse(files.find((f) => f.name === 'classes/2026_2-DCC219/class.json').data);
  assert.equal(cls.turma, undefined);                  // no class-level turma
  assert.equal(cls.roster[0].turma, 'TE1');            // turma is on the roster entry
});
