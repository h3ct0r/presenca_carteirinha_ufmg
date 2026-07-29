// Pure, DOM-free matching of Moodle photo filenames → student ids by name.
// See docs/software/STUDENT_PHOTOS.md §5. The photo filename and the student
// `name` both come from the same UFMG source (the Diário `NOME`), so exact
// name-key matches dominate; fuzzy + a review UI (in app.js) catch the rest.

// Portuguese name particles dropped for the "loose" key (ANA DA SILVA ~ ANA SILVA).
const PARTICLES = new Set(['DE', 'DA', 'DO', 'DAS', 'DOS', 'E']);

const IMG_EXT = /\.(jpe?g|png|gif|webp|bmp)$/i;

export function stripExt(filename) {
  return String(filename).replace(IMG_EXT, '');
}

// Canonical key: deaccent, uppercase, underscores/whitespace → single spaces,
// drop punctuation, trim.
export function nameKey(s) {
  return String(s)
    .normalize('NFD')
    .replace(/[̀-ͯ]/g, '')  // strip combining marks (accents)
    .toUpperCase()
    .replace(/[_\s]+/g, ' ')
    .replace(/[^A-Z0-9 ]/g, '')
    .trim();
}

// Looser key: canonical, with Portuguese particles removed.
export function looseKey(s) {
  return nameKey(s)
    .split(' ')
    .filter((w) => w && !PARTICLES.has(w))
    .join(' ');
}

// Jaccard overlap of the (particle-stripped) word tokens of two names, 0..1.
export function tokenSetSimilarity(a, b) {
  const ta = new Set(looseKey(a).split(' ').filter(Boolean));
  const tb = new Set(looseKey(b).split(' ').filter(Boolean));
  if (ta.size === 0 || tb.size === 0) return 0;
  let inter = 0;
  for (const t of ta) if (tb.has(t)) inter++;
  const union = ta.size + tb.size - inter;
  return union ? inter / union : 0;
}

const FUZZY_THRESHOLD = 0.6;  // min token-set overlap to *suggest* (never auto).

function indexBy(students, keyFn) {
  const m = new Map();
  for (const s of students) {
    const k = keyFn(s.name);
    if (!k) continue;
    if (!m.has(k)) m.set(k, []);
    m.get(k).push(s);
  }
  return m;
}

// filenames: string[]; students: [{ id, name }, ...].
// Returns { matched, review, unmatched, studentsWithoutPhoto } where:
//   matched  = [{ filename, id, name, confidence:'exact' }]           (auto)
//   review   = [{ filename, reason, candidates:[{id,name}], suggestion:id|null }]
//   unmatched= [{ filename }]
//   studentsWithoutPhoto = [{ id, name }]   (no exact/auto match yet)
export function matchPhotos(filenames, students) {
  const byName = indexBy(students, nameKey);
  const byLoose = indexBy(students, looseKey);

  const matched = [];
  const review = [];
  const unmatched = [];
  const usedIds = new Set();

  for (const filename of filenames) {
    const stem = stripExt(filename);
    const exact = byName.get(nameKey(stem)) || [];

    if (exact.length === 1) {
      matched.push({ filename, id: exact[0].id, name: exact[0].name, confidence: 'exact' });
      usedIds.add(exact[0].id);
      continue;
    }
    if (exact.length > 1) {
      review.push({
        filename, reason: 'ambiguous',
        candidates: exact.map((s) => ({ id: s.id, name: s.name })), suggestion: null,
      });
      continue;
    }

    const loose = byLoose.get(looseKey(stem)) || [];
    if (loose.length === 1) {
      review.push({
        filename, reason: 'suggested',
        candidates: [{ id: loose[0].id, name: loose[0].name }], suggestion: loose[0].id,
      });
      continue;
    }
    if (loose.length > 1) {
      review.push({
        filename, reason: 'ambiguous',
        candidates: loose.map((s) => ({ id: s.id, name: s.name })), suggestion: null,
      });
      continue;
    }

    // Fuzzy fallback: best token-set overlap above the threshold → suggest.
    let best = null;
    let bestScore = 0;
    for (const s of students) {
      const score = tokenSetSimilarity(stem, s.name);
      if (score > bestScore) { bestScore = score; best = s; }
    }
    if (best && bestScore >= FUZZY_THRESHOLD) {
      review.push({
        filename, reason: 'suggested',
        candidates: [{ id: best.id, name: best.name }], suggestion: best.id,
      });
    } else {
      unmatched.push({ filename });
    }
  }

  const studentsWithoutPhoto = students
    .filter((s) => !usedIds.has(s.id))
    .map((s) => ({ id: s.id, name: s.name }));

  return { matched, review, unmatched, studentsWithoutPhoto };
}
