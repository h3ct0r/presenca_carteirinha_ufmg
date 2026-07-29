import { test } from 'node:test';
import assert from 'node:assert/strict';
import {
  STORAGE_KEY, STORAGE_SCHEMA, encodeModel, decodeModel, modelHasContent, describeSavedAt,
} from '../src/persist.js';

const MODEL = {
  teachers: [{ name: 'Prof', email: 'p@x.edu', rfid_uid: '', password: '1234' }],
  students: [{ id: '2023-0142', name: 'Maria Santos', rfid_uid: '' }],
  classes: [{ code: 'CS101', name: 'DS', schedule: '', teacher_emails: ['p@x.edu'],
              color: '272766', roster: [{ id: '2023-0142', turma: 'TE1' }] }],
};

test('a model survives an encode/decode round trip', () => {
  const got = decodeModel(encodeModel(MODEL, 1700000000000));
  assert.deepEqual(got.model, MODEL);
  assert.equal(got.savedAt, 1700000000000);
});

test('the stored payload is versioned and self-describing', () => {
  const raw = JSON.parse(encodeModel(MODEL));
  assert.equal(raw.schema, STORAGE_SCHEMA);
  assert.ok(raw.savedAt > 0);
  assert.ok(raw.model);
});

test('the storage key is namespaced so it cannot collide with another app', () => {
  assert.match(STORAGE_KEY, /^presenca-carteirinha\./);
});

// Everything below must return null rather than throw: a bad stored value must
// never stop the page from loading.
test('nothing stored yet decodes to null', () => {
  assert.equal(decodeModel(null), null);
  assert.equal(decodeModel(''), null);
  assert.equal(decodeModel(undefined), null);
});

test('corrupt JSON decodes to null instead of throwing', () => {
  assert.equal(decodeModel('{ not json'), null);
  assert.equal(decodeModel('[1,2,3]'), null);
  assert.equal(decodeModel('"a string"'), null);
});

test('a payload from a different schema is discarded, not half-read', () => {
  const future = JSON.stringify({ schema: STORAGE_SCHEMA + 1, savedAt: 1, model: MODEL });
  assert.equal(decodeModel(future), null);
  const legacy = JSON.stringify({ savedAt: 1, model: MODEL });  // no schema at all
  assert.equal(decodeModel(legacy), null);
});

test('a payload whose model is not an object is rejected', () => {
  for (const bad of [null, 42, 'x', []]) {
    const raw = JSON.stringify({ schema: STORAGE_SCHEMA, savedAt: 1, model: bad });
    assert.equal(decodeModel(raw), null, `model=${JSON.stringify(bad)} must be rejected`);
  }
});

test('modelHasContent only reports true when something was authored', () => {
  assert.equal(modelHasContent({ teachers: [], students: [], classes: [] }), false);
  assert.equal(modelHasContent({}), false);
  assert.equal(modelHasContent(null), false);
  assert.equal(modelHasContent({ teachers: [{}], students: [], classes: [] }), true);
  assert.equal(modelHasContent({ teachers: [], students: [], classes: [{}] }), true);
});

test('describeSavedAt reads as a recency, then falls back to a timestamp', () => {
  const now = 1700000000000;
  assert.equal(describeSavedAt(0, now), '');
  assert.equal(describeSavedAt(now - 2000, now), 'just now');
  assert.equal(describeSavedAt(now - 30000, now), '30s ago');
  assert.equal(describeSavedAt(now - 5 * 60000, now), '5 min ago');
  assert.match(describeSavedAt(now - 5 * 3600000, now), /^\d{4}-\d{2}-\d{2} \d{2}:\d{2}$/);
});
