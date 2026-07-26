import { test } from 'node:test';
import assert from 'node:assert/strict';
import { uidNormalize } from '../src/uid.js';

// These must agree with the firmware's uid_normalize() (src/app/uid.cpp):
// uppercase, strip ':' '-' ' '.

test('uppercases hex', () => {
  assert.equal(uidNormalize('e0d1335f'), 'E0D1335F');
});

test('strips colons, dashes and spaces', () => {
  assert.equal(uidNormalize('E0:D1:33:5F'), 'E0D1335F');
  assert.equal(uidNormalize('e0-d1-33-5f'), 'E0D1335F');
  assert.equal(uidNormalize('e0 d1 33 5f'), 'E0D1335F');
});

test('mixed separators and case collapse to one canonical form', () => {
  assert.equal(uidNormalize('e0:D1-33 5f'), 'E0D1335F');
});

test('two different-looking UIDs that are the same card collide', () => {
  assert.equal(uidNormalize('E0:D1:33:5F'), uidNormalize('e0d1335f'));
});

test('distinct cards stay distinct', () => {
  assert.notEqual(uidNormalize('E0:D1:33:5F'), uidNormalize('E0:D1:33:6F'));
});

test('empty and nullish inputs normalize to empty string', () => {
  assert.equal(uidNormalize(''), '');
  assert.equal(uidNormalize(null), '');
  assert.equal(uidNormalize(undefined), '');
});
