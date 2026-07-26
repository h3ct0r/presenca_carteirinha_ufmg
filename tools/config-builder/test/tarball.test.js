import { test } from 'node:test';
import assert from 'node:assert/strict';
import { makeTar } from '../src/tarball.js';

const dec = new TextDecoder();
const field = (buf, off, len) => dec.decode(buf.subarray(off, off + len)).replace(/\0.*$/, '').trim();

test('archive length is a multiple of 512', () => {
  const tar = makeTar([{ name: 'config.json', data: 'hello' }]);
  assert.equal(tar.length % 512, 0);
});

test('header magic is ustar', () => {
  const tar = makeTar([{ name: 'config.json', data: 'hi' }]);
  assert.equal(field(tar, 257, 6), 'ustar');
});

test('name and octal size are written correctly', () => {
  const data = 'hello world'; // 11 bytes
  const tar = makeTar([{ name: 'students/students.json', data }]);
  assert.equal(field(tar, 0, 100), 'students/students.json');
  assert.equal(parseInt(field(tar, 124, 12), 8), 11); // octal "00000000013"
});

test('typeflag is regular file', () => {
  const tar = makeTar([{ name: 'config.json', data: 'x' }]);
  assert.equal(tar[156], 0x30); // '0'
});

test('checksum equals the summed header bytes (with the field as spaces)', () => {
  const tar = makeTar([{ name: 'config.json', data: 'payload' }]);
  const header = tar.subarray(0, 512);
  // Recompute: read the stored checksum, then sum the header with the
  // checksum field replaced by 8 spaces.
  const stored = parseInt(field(tar, 148, 8), 8);
  let sum = 0;
  for (let i = 0; i < 512; i++) {
    sum += (i >= 148 && i < 156) ? 0x20 : header[i];
  }
  assert.equal(stored, sum);
});

test('two trailing zero blocks terminate the archive', () => {
  const tar = makeTar([{ name: 'config.json', data: 'x' }]);
  const tail = tar.subarray(tar.length - 1024);
  assert.ok(tail.every((b) => b === 0));
});

test('round-trip: size field parses back to the payload length for each entry', () => {
  const files = [
    { name: 'config.json', data: 'a'.repeat(37) },
    { name: 'classes/CS101-M1/class.json', data: 'b'.repeat(600) },
  ];
  const tar = makeTar(files);
  // Walk the blocks and read each header's name+size.
  let off = 0;
  const seen = [];
  while (off < tar.length) {
    const block = tar.subarray(off, off + 512);
    if (block.every((b) => b === 0)) break; // EOF
    const name = field(tar, off, 100);
    const size = parseInt(field(tar, off + 124, 12), 8);
    seen.push({ name, size });
    off += 512 + Math.ceil(size / 512) * 512;
  }
  assert.deepEqual(seen, [
    { name: 'config.json', size: 37 },
    { name: 'classes/CS101-M1/class.json', size: 600 },
  ]);
});
