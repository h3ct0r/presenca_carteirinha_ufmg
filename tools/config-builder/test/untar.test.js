import { test } from 'node:test';
import assert from 'node:assert/strict';
import { makeTar } from '../src/tarball.js';
import { parseTar } from '../src/untar.js';

const enc = new TextEncoder();
const dec = new TextDecoder();

test('round-trips a single text file through makeTar', () => {
  const tar = makeTar([{ name: 'config.json', data: '{"x":1}' }]);
  const files = parseTar(tar);
  assert.equal(files.length, 1);
  assert.equal(files[0].name, 'config.json');
  assert.equal(dec.decode(files[0].data), '{"x":1}');
});

test('round-trips multiple entries in order, preserving binary bytes', () => {
  const bin = new Uint8Array([0xff, 0xd8, 0xff, 0x00, 0x10, 0x42]);  // JPEG-ish
  const tar = makeTar([
    { name: 'students/students.json', data: 'roster' },
    { name: 'students/photos/2025115525.jpg', data: bin },
  ]);
  const files = parseTar(tar);
  assert.equal(files.length, 2);
  assert.equal(files[0].name, 'students/students.json');
  assert.equal(files[1].name, 'students/photos/2025115525.jpg');
  assert.deepEqual([...files[1].data], [...bin]);
});

test('handles a payload that exactly fills a 512 block (no off-by-one)', () => {
  const data = new Uint8Array(512).fill(0x41);
  const tar = makeTar([{ name: 'a.jpg', data }]);
  const files = parseTar(tar);
  assert.equal(files.length, 1);
  assert.equal(files[0].data.length, 512);
  assert.deepEqual([...files[0].data], [...data]);
});

test('empty archive (two zero blocks) yields no entries', () => {
  const tar = makeTar([]);
  assert.deepEqual(parseTar(tar), []);
});

test('skips directory entries, returns only regular files', () => {
  // Hand-build a tar with a directory entry (typeflag 5) before a file.
  const file = makeTar([{ name: 'students/photos/x.jpg', data: 'x' }]);
  // Craft a dir header block.
  const dir = new Uint8Array(512);
  const put = (s, off, len) => dir.set(enc.encode(s).subarray(0, len), off);
  put('students/photos/', 0, 100);
  put('0000755', 100, 7);
  put('00000000000', 124, 11);   // size 0
  for (let i = 148; i < 156; i++) dir[i] = 0x20;
  dir[156] = 0x35;               // typeflag '5' = directory
  put('ustar', 257, 5);
  dir[263] = 0x30; dir[264] = 0x30;
  let sum = 0;
  for (const b of dir) sum += b;
  put(sum.toString(8).padStart(6, '0') + '\0 ', 148, 8);

  const combined = new Uint8Array(dir.length + file.length);
  combined.set(dir, 0);
  combined.set(file, dir.length);
  const files = parseTar(combined);
  assert.equal(files.length, 1);
  assert.equal(files[0].name, 'students/photos/x.jpg');
});

test('rejects a non-tar buffer', () => {
  const junk = new Uint8Array(512).fill(0x7a);
  assert.throws(() => parseTar(junk), /ustar/);
});

test('accepts an ArrayBuffer as well as a Uint8Array', () => {
  const tar = makeTar([{ name: 'config.json', data: 'ok' }]);
  const files = parseTar(tar.buffer.slice(0));
  assert.equal(dec.decode(files[0].data), 'ok');
});
