// Pure, DOM-free encode/decode for the browser-stored working copy of the
// authoring model, so a reload continues where you left off.
//
// Only the JSON model is stored. Student PHOTOS are deliberately NOT persisted:
// they are binary, easily several MB, and localStorage is a ~5 MB string store —
// re-import the Moodle tar each session (STUDENT_PHOTOS.md §7).
//
// The payload is versioned. A stored copy written by an older/newer schema is
// discarded rather than half-read, so a format change can never resurrect a
// model the current validator would mis-handle.

export const STORAGE_KEY = 'presenca-carteirinha.config-builder.model';
export const STORAGE_SCHEMA = 1;

// model -> string ready for localStorage.setItem.
export function encodeModel(model, now = Date.now()) {
  return JSON.stringify({ schema: STORAGE_SCHEMA, savedAt: now, model });
}

// string -> { model, savedAt } , or null when there is nothing usable to
// restore (absent, corrupt, wrong schema, or not an object). Never throws:
// a bad stored value must not stop the page from loading.
export function decodeModel(text) {
  if (typeof text !== 'string' || !text) return null;
  let parsed;
  try {
    parsed = JSON.parse(text);
  } catch {
    return null;
  }
  if (!parsed || typeof parsed !== 'object') return null;
  if (parsed.schema !== STORAGE_SCHEMA) return null;  // different format: ignore
  const model = parsed.model;
  if (!model || typeof model !== 'object' || Array.isArray(model)) return null;
  return { model, savedAt: Number(parsed.savedAt) || 0 };
}

// True when the model holds anything worth saving, so an untouched page never
// writes an empty record over a previous session's work.
export function modelHasContent(model) {
  if (!model || typeof model !== 'object') return false;
  return ['teachers', 'students', 'classes'].some(
    (k) => Array.isArray(model[k]) && model[k].length > 0,
  );
}

// "just now" / "3 min ago" / "2026-07-29 14:05" for the saved-state hint.
export function describeSavedAt(savedAt, now = Date.now()) {
  if (!savedAt) return '';
  const secs = Math.max(0, Math.round((now - savedAt) / 1000));
  if (secs < 10) return 'just now';
  if (secs < 60) return `${secs}s ago`;
  const mins = Math.round(secs / 60);
  if (mins < 60) return `${mins} min ago`;
  const d = new Date(savedAt);
  const pad = (n) => String(n).padStart(2, '0');
  return `${d.getFullYear()}-${pad(d.getMonth() + 1)}-${pad(d.getDate())} ` +
         `${pad(d.getHours())}:${pad(d.getMinutes())}`;
}
