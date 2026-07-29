import { test } from 'node:test';
import assert from 'node:assert/strict';
import { nameKey, looseKey, tokenSetSimilarity, stripExt, matchPhotos } from '../src/photomatch.js';

test('nameKey deaccents, uppercases, and normalizes separators', () => {
  assert.equal(nameKey('Caio Otávio Messias'), 'CAIO OTAVIO MESSIAS');
  assert.equal(nameKey('MARIA_SANTOS'), 'MARIA SANTOS');
  assert.equal(nameKey('  josé   da   silva '), 'JOSE DA SILVA');
  assert.equal(nameKey('Ana-Júlia (Ferreira)'), 'ANAJULIA FERREIRA');
});

test('looseKey drops Portuguese particles', () => {
  assert.equal(looseKey('ANA DA SILVA'), 'ANA SILVA');
  assert.equal(looseKey('João de Souza e Costa'), 'JOAO SOUZA COSTA');
});

test('stripExt removes image extensions case-insensitively', () => {
  assert.equal(stripExt('MARIA_SANTOS.jpg'), 'MARIA_SANTOS');
  assert.equal(stripExt('foo.JPEG'), 'foo');
  assert.equal(stripExt('bar.png'), 'bar');
  assert.equal(stripExt('nodot'), 'nodot');
});

test('tokenSetSimilarity is 1 for reordered/particle-only differences', () => {
  assert.equal(tokenSetSimilarity('ANA DA SILVA', 'SILVA ANA'), 1);
  assert.ok(tokenSetSimilarity('ANA SILVA', 'ANA SILVA SOUZA') > 0.5);
  assert.equal(tokenSetSimilarity('ANA SILVA', 'BRUNO COSTA'), 0);
});

const STUDENTS = [
  { id: '2025115525', name: 'Maria Santos' },
  { id: '2018107393', name: 'Caio Otávio Messias' },
  { id: '2020000001', name: 'Ana da Silva' },
  { id: '2020000002', name: 'João de Souza' },
];

test('exact unique name → auto matched, re-keyed to id', () => {
  const r = matchPhotos(['MARIA_SANTOS.jpg', 'CAIO_OTAVIO_MESSIAS.jpg'], STUDENTS);
  assert.equal(r.matched.length, 2);
  const maria = r.matched.find((m) => m.filename === 'MARIA_SANTOS.jpg');
  assert.equal(maria.id, '2025115525');
  assert.equal(maria.confidence, 'exact');
  assert.equal(r.review.length, 0);
  assert.equal(r.unmatched.length, 0);
});

test('accented source name matches a deaccented filename exactly', () => {
  const r = matchPhotos(['CAIO_OTAVIO_MESSIAS.jpg'], STUDENTS);
  assert.equal(r.matched.length, 1);
  assert.equal(r.matched[0].id, '2018107393');
});

test('particle difference falls to loose key as a suggestion (needs review)', () => {
  const r = matchPhotos(['ANA_SILVA.jpg'], STUDENTS);   // roster: "Ana da Silva"
  assert.equal(r.matched.length, 0);
  assert.equal(r.review.length, 1);
  assert.equal(r.review[0].reason, 'suggested');
  assert.equal(r.review[0].suggestion, '2020000001');
});

test('duplicate roster names → ambiguous review with all candidates', () => {
  const dup = [
    { id: '1', name: 'Maria Santos' },
    { id: '2', name: 'Maria Santos' },
  ];
  const r = matchPhotos(['MARIA_SANTOS.jpg'], dup);
  assert.equal(r.matched.length, 0);
  assert.equal(r.review.length, 1);
  assert.equal(r.review[0].reason, 'ambiguous');
  assert.equal(r.review[0].candidates.length, 2);
  assert.equal(r.review[0].suggestion, null);
});

test('no plausible match → unmatched', () => {
  const r = matchPhotos(['ZZZ_UNKNOWN_PERSON.jpg'], STUDENTS);
  assert.equal(r.unmatched.length, 1);
  assert.equal(r.unmatched[0].filename, 'ZZZ_UNKNOWN_PERSON.jpg');
});

test('studentsWithoutPhoto lists only students with no auto match', () => {
  const r = matchPhotos(['MARIA_SANTOS.jpg'], STUDENTS);
  const ids = r.studentsWithoutPhoto.map((s) => s.id).sort();
  assert.deepEqual(ids, ['2018107393', '2020000001', '2020000002']);
});
