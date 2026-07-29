// Pure, DOM-free ustar *reader* — the counterpart to tarball.js's writer. Used
// to parse a Moodle student-photos `.tar` in the browser (or Node tests) into a
// list of file entries. See docs/software/STUDENT_PHOTOS.md §4.
//
// Scope: uncompressed POSIX ustar (what `tar` and Moodle produce). Regular-file
// entries only; directories and metadata entries (pax/global/GNU long-name) are
// skipped. GNU long names (>100 chars) are not reconstructed — student photo
// names are short, so this is fine; such an entry is simply skipped, not
// mis-parsed. A `.tar.gz` must be decompressed before calling this.

const dec = new TextDecoder();

function str(bytes, off, len) {
  // NUL-terminated field: read up to the first NUL within [off, off+len).
  let end = off;
  const limit = off + len;
  while (end < limit && bytes[end] !== 0) end++;
  return dec.decode(bytes.subarray(off, end));
}

// Parse a tar `size` field: usually octal ASCII; tolerate leading spaces and an
// empty field (0). (Base-256 GNU large sizes aren't needed for small photos.)
function octal(bytes, off, len) {
  let n = 0;
  const limit = off + len;
  for (let i = off; i < limit; i++) {
    const c = bytes[i];
    if (c === 0 || c === 0x20) continue;      // NUL / space padding
    if (c < 0x30 || c > 0x37) continue;       // ignore anything non-octal
    n = n * 8 + (c - 0x30);
  }
  return n;
}

function isZeroBlock(bytes, off) {
  for (let i = 0; i < 512; i++) if (bytes[off + i] !== 0) return false;
  return true;
}

// Parse `bytes` (Uint8Array / ArrayBuffer) → [{ name, data }] for regular files.
// `data` is a Uint8Array view into a copy (safe to keep). Throws on a clearly
// truncated / non-tar buffer.
export function parseTar(input) {
  const bytes = input instanceof Uint8Array ? input : new Uint8Array(input);
  const out = [];
  let off = 0;

  while (off + 512 <= bytes.length) {
    if (isZeroBlock(bytes, off)) break;       // end-of-archive marker

    // ustar magic lives at offset 257 ("ustar"); be lenient (GNU writes "ustar ").
    const magic = str(bytes, off + 257, 5);
    if (magic !== 'ustar') {
      throw new Error('not a ustar archive (bad magic at block ' + off / 512 + ')');
    }

    const name = str(bytes, off + 0, 100);
    const prefix = str(bytes, off + 345, 155);  // ustar prefix for long paths
    const full = prefix ? prefix + '/' + name : name;
    const size = octal(bytes, off + 124, 12);
    const typeflag = bytes[off + 156];          // '0' or 0 = regular file
    const dataOff = off + 512;

    if (dataOff + size > bytes.length) throw new Error('truncated tar payload');

    const isRegular = typeflag === 0x30 || typeflag === 0;
    if (isRegular && full) {
      out.push({ name: full, data: bytes.slice(dataOff, dataOff + size) });
    }

    off = dataOff + Math.ceil(size / 512) * 512;  // skip payload + padding
  }
  return out;
}
