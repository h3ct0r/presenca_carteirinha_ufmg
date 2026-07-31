import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { validate, LIMITS, CHECKIN_MODES, checkinMode } from '../src/validate.js';
import {
  buildClass, DEFAULT_FACE_VERIFY_SECONDS, DEFAULT_MIN_ATTENDANCE_MIN,
} from '../src/model.js';

// The per-class attendance settings the builder authors (CONFIG_IMPORT.md §3.3):
// check-in mode (single / double / photo) plus the two numbers it needs.

const FIXTURE = JSON.parse(
  readFileSync(fileURLToPath(new URL('../fixtures/example.model.json', import.meta.url)), 'utf8'),
);
const clone = (m) => JSON.parse(JSON.stringify(m));
const has = (errs, part) => errs.some((e) => e.message.includes(part));

// --- emitted class.json ----------------------------------------------------

test('a class that never touched these settings emits the contract defaults', () => {
  const out = buildClass({ code: 'X', name: 'X', roster: [] });
  assert.equal(out.capture_photos, false);
  assert.equal(out.timed_attendance, false);
  assert.equal(out.face_verify_seconds, DEFAULT_FACE_VERIFY_SECONDS);
  assert.equal(out.min_attendance_min, DEFAULT_MIN_ATTENDANCE_MIN);
});

test('the defaults match the firmware fallbacks (15 s / 45 min)', () => {
  assert.equal(DEFAULT_FACE_VERIFY_SECONDS, 15);
  assert.equal(DEFAULT_MIN_ATTENDANCE_MIN, 45);
  assert.equal(DEFAULT_FACE_VERIFY_SECONDS, LIMITS.FACE_VERIFY_SECONDS_DEFAULT);
  assert.equal(DEFAULT_MIN_ATTENDANCE_MIN, LIMITS.MIN_ATTENDANCE_DEFAULT);
});

test('authored values are emitted verbatim', () => {
  const out = buildClass({
    code: 'X', name: 'X', roster: [],
    timed_attendance: true, min_attendance_min: 30,
    capture_photos: true, face_verify_seconds: 20,
  });
  assert.equal(out.timed_attendance, true);
  assert.equal(out.min_attendance_min, 30);
  assert.equal(out.capture_photos, true);
  assert.equal(out.face_verify_seconds, 20);
});

test('a blank number falls back to the default instead of emitting ""', () => {
  const out = buildClass({ code: 'X', name: 'X', roster: [], min_attendance_min: '', face_verify_seconds: '' });
  assert.equal(out.min_attendance_min, DEFAULT_MIN_ATTENDANCE_MIN);
  assert.equal(out.face_verify_seconds, DEFAULT_FACE_VERIFY_SECONDS);
});

test('a numeric string is emitted as a number (JSON must not carry "45")', () => {
  const out = buildClass({ code: 'X', name: 'X', roster: [], min_attendance_min: '30' });
  assert.equal(out.min_attendance_min, 30);
  assert.equal(typeof out.min_attendance_min, 'number');
});

// An out-of-range value must survive into buildClass unchanged: silently
// clamping here would hide the mistake that validate() is supposed to report.
test('an out-of-range value is not silently clamped', () => {
  const out = buildClass({ code: 'X', name: 'X', roster: [], face_verify_seconds: 999 });
  assert.equal(out.face_verify_seconds, 999);
  assert.ok(has(validate(withClass({ face_verify_seconds: 999 })), 'photo countdown must be between'));
});

// --- mode mapping ----------------------------------------------------------

test('each mode maps to the pair of device booleans', () => {
  assert.deepEqual(CHECKIN_MODES.single, { timed_attendance: false, capture_photos: false });
  assert.deepEqual(CHECKIN_MODES.double, { timed_attendance: true, capture_photos: false });
  assert.deepEqual(CHECKIN_MODES.photo, { timed_attendance: false, capture_photos: true });
});

test('checkinMode reads the booleans back', () => {
  assert.equal(checkinMode({}), 'single');
  assert.equal(checkinMode({ timed_attendance: true }), 'double');
  assert.equal(checkinMode({ capture_photos: true }), 'photo');
});

// The device can hold both flags (set in its ⚙ settings); a model loaded from
// such a card must not silently read as plain single-tap.
test('a class carrying both flags reports photo, not single', () => {
  assert.equal(checkinMode({ timed_attendance: true, capture_photos: true }), 'photo');
});

test('every mode round-trips through the booleans', () => {
  for (const [name, flags] of Object.entries(CHECKIN_MODES)) {
    assert.equal(checkinMode(flags), name, `${name} did not round-trip`);
  }
});

// --- validation ------------------------------------------------------------

function withClass(extra) {
  const m = clone(FIXTURE);
  Object.assign(m.classes[0], extra);
  return m;
}

test('the fixture stays valid with these settings absent', () => {
  assert.deepEqual(validate(FIXTURE), []);
});

test('in-range values pass', () => {
  assert.deepEqual(validate(withClass({ face_verify_seconds: 3, min_attendance_min: 1 })), []);
  assert.deepEqual(validate(withClass({ face_verify_seconds: 60, min_attendance_min: 600 })), []);
});

test('face_verify_seconds outside 3..60 is rejected', () => {
  assert.ok(has(validate(withClass({ face_verify_seconds: 2 })), 'photo countdown must be between 3 and 60'));
  assert.ok(has(validate(withClass({ face_verify_seconds: 61 })), 'photo countdown must be between 3 and 60'));
});

test('min_attendance_min outside 1..600 is rejected', () => {
  assert.ok(has(validate(withClass({ min_attendance_min: 0 })), 'double-tap threshold must be between 1 and 600'));
  assert.ok(has(validate(withClass({ min_attendance_min: 601 })), 'double-tap threshold must be between 1 and 600'));
});

test('a fractional or non-numeric value is rejected', () => {
  assert.ok(has(validate(withClass({ min_attendance_min: 45.5 })), 'must be a whole number'));
  assert.ok(has(validate(withClass({ face_verify_seconds: 'soon' })), 'must be a whole number'));
});

test('an absent or blank setting is not an error (the device defaults)', () => {
  assert.deepEqual(validate(withClass({ face_verify_seconds: undefined })), []);
  assert.deepEqual(validate(withClass({ min_attendance_min: '' })), []);
  assert.deepEqual(validate(withClass({ min_attendance_min: null })), []);
});

test('the error names the class and the offending field', () => {
  const errs = validate(withClass({ min_attendance_min: 9999 }));
  const e = errs.find((x) => x.field === 'min_attendance_min');
  assert.ok(e, 'no error tagged with the field');
  assert.equal(e.scope, 'class');
  assert.ok(e.message.startsWith(FIXTURE.classes[0].code));
});
