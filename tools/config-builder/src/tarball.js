// Pure, DOM-free ustar tar writer. See docs/CONFIG_IMPORT.md §4.
//
// Emits an uncompressed POSIX ustar archive. Entry names must already be
// whitelist-clean (no leading '/', no '..') — model.js only ever produces the
// three authored paths, and validate.js rejects anything else before export.

// TextEncoder is a global in browsers and in Node 18+.
const enc = new TextEncoder();

// files: [{ name: string, data: Uint8Array | string }, ...]
// returns Uint8Array (the full archive, a multiple of 512 bytes).
export function makeTar(files) {
  const blocks = [];
  const pad = (buf) => {
    const r = buf.length % 512;
    if (r) blocks.push(new Uint8Array(512 - r));
  };

  for (const f of files) {
    const data = typeof f.data === 'string' ? enc.encode(f.data) : f.data;
    const h = new Uint8Array(512);
    const put = (str, off, len) => h.set(enc.encode(str).subarray(0, len), off);

    put(f.name, 0, 100);                    // name
    put('0000644', 100, 7);                 // mode
    put('0000000', 108, 7);                 // uid
    put('0000000', 116, 7);                 // gid
    put(data.length.toString(8).padStart(11, '0'), 124, 11);  // size (octal)
    put('00000000000', 136, 11);            // mtime (0 — the device has no RTC)
    put('        ', 148, 8);                // checksum field = spaces during calc
    h[156] = 0x30;                          // typeflag '0' = regular file
    put('ustar', 257, 5);                   // magic
    h[263] = 0x30; h[264] = 0x30;           // version "00"

    let sum = 0;
    for (const b of h) sum += b;            // checksum = sum of all header bytes
    // 6 octal digits, then NUL and a space (the conventional ustar layout).
    put(sum.toString(8).padStart(6, '0') + '\0 ', 148, 8);

    blocks.push(h, data);
    pad(data);
  }

  blocks.push(new Uint8Array(1024));        // two zero blocks = end of archive

  const total = blocks.reduce((n, b) => n + b.length, 0);
  const out = new Uint8Array(total);
  let o = 0;
  for (const b of blocks) { out.set(b, o); o += b.length; }
  return out;
}
