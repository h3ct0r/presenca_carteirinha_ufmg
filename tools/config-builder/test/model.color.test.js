import { test } from 'node:test';
import assert from 'node:assert/strict';
import { CLASS_COLORS, pickClassColor } from '../src/model.js';
import { parseDiario, applyDiario } from '../src/diario.js';
import { LIMITS } from '../src/validate.js';

// The device paints the class initial in WHITE on this colour
// (scr_classes.cpp), so a light entry would be unreadable. Guard the palette.
function contrastVsWhite(hex) {
  const chan = (i) => {
    const c = parseInt(hex.slice(i, i + 2), 16) / 255;
    return c <= 0.03928 ? c / 12.92 : ((c + 0.055) / 1.055) ** 2.4;
  };
  const lum = 0.2126 * chan(0) + 0.7152 * chan(2) + 0.0722 * chan(4);
  return 1.05 / (lum + 0.05);
}

test('every palette colour is legible under white text (WCAG AA)', () => {
  for (const c of CLASS_COLORS) {
    assert.match(c, /^[0-9A-F]{6}$/, `${c} must be bare 6-hex uppercase`);
    const ratio = contrastVsWhite(c);
    assert.ok(ratio >= 4.5, `${c} contrast vs white is ${ratio.toFixed(2)}, need >= 4.5`);
  }
});

test('the palette has no duplicates and covers the class cap', () => {
  assert.equal(new Set(CLASS_COLORS).size, CLASS_COLORS.length);
  assert.ok(CLASS_COLORS.length >= LIMITS.MAX_CLASSES,
    'need at least one colour per class so each can be unique');
});

test('pickClassColor avoids colours already in use', () => {
  const used = CLASS_COLORS.slice(0, CLASS_COLORS.length - 1);
  // Whatever the rng returns, the only free colour must come back.
  for (const r of [0, 0.25, 0.5, 0.99]) {
    assert.equal(pickClassColor(used, () => r), CLASS_COLORS[CLASS_COLORS.length - 1]);
  }
});

test('pickClassColor ignores "#" and case when comparing used colours', () => {
  const used = ['#' + CLASS_COLORS[0].toLowerCase()];
  const got = pickClassColor(used, () => 0);
  assert.notEqual(got, CLASS_COLORS[0]);
});

test('pickClassColor still returns a valid colour when all are taken', () => {
  const got = pickClassColor(CLASS_COLORS, () => 0.7);
  assert.ok(CLASS_COLORS.includes(got));
});

const csv = (course) => `PERIODO;ATIVIDADE;TURMA
2026/2;${course};TE1

N.;MATRICULA;NOME;PTS OBTIDOS
1;2025115525;ALIGIA CASSIO;0,00
`;

test('importing several classes gives each a different colour', () => {
  const model = { teachers: [], students: [], classes: [] };
  // A fixed rng would collide constantly; the "skip used" rule must still
  // hand out distinct colours.
  for (const course of ['DCC219', 'DCC003', 'DCC111', 'DCC888']) {
    applyDiario(model, parseDiario(csv(course)), () => 0);
  }
  const colors = model.classes.map((c) => c.color);
  assert.equal(colors.length, 4);
  assert.equal(new Set(colors).size, 4, 'imported classes must not share a colour');
  for (const c of colors) assert.ok(CLASS_COLORS.includes(c));
});

test('re-importing a class does not recolour it', () => {
  const model = { teachers: [], students: [], classes: [] };
  applyDiario(model, parseDiario(csv('DCC219')));
  const first = model.classes[0].color;
  applyDiario(model, parseDiario(csv('DCC219')));
  assert.equal(model.classes.length, 1);
  assert.equal(model.classes[0].color, first);
});

test('an author-chosen colour is never overwritten', () => {
  const model = {
    teachers: [], students: [],
    classes: [{ code: '2026_2-DCC219', name: '', schedule: '', teacher_emails: [], color: 'ABCDEF', roster: [] }],
  };
  applyDiario(model, parseDiario(csv('DCC219')));
  assert.equal(model.classes[0].color, 'ABCDEF');
});
