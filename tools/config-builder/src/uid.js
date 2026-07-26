// Port of the firmware's uid_normalize() (src/app/uid.cpp): uppercase every
// character and strip the separators the readers emit (':', '-', ' ').
//
// The builder's UID uniqueness checks MUST normalize identically to the device,
// or two "different-looking" UIDs that collide on-device would pass here and
// fail the import. Keep this file in lockstep with src/app/uid.cpp.

export function uidNormalize(input) {
  if (input == null) return '';
  let out = '';
  for (const ch of String(input)) {
    if (ch === ':' || ch === '-' || ch === ' ') continue;
    out += ch.toUpperCase();
  }
  return out;
}
