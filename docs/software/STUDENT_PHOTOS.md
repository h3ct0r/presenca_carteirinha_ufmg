# Student photos

Student avatars go from a Moodle export to the device screen in three steps:

1. **Download** the class photos from Moodle as a `.tar` of `NAME.jpg` files —
   see [MOODLE_PHOTOS.md](MOODLE_PHOTOS.md).
2. **Match and bundle** them in the config-builder: each filename is matched to a
   student by name, re-keyed to their matrícula, re-encoded, and packed into
   `config.tar` as `students/photos/<id>.jpg`.
3. **Import and display** — the device applies the avatars and shows them on
   every check-in.

Shipped and verified on device. The tar contract is
[CONFIG_IMPORT.md](CONFIG_IMPORT.md); the on-card paths are in
[SD_CARD.md](SD_CARD.md).

## Matching filenames to students

The photo filename and the student's `name` come from the **same UFMG source**
(the `NOME` column of the Diário CSV), so name-to-name matching hits high.
Matching happens in the browser — never on the device.

A canonical key is built from both the filename (extension stripped) and each
`student.name`:

```
NFD-decompose → strip combining marks (deaccent)
uppercase
'_' and runs of whitespace → single space
drop everything outside [A-Z0-9 ]
trim
```

A looser key additionally drops Portuguese particles (`DE DA DO DAS DOS E`), so
"ANA DA SILVA" still matches "ANA SILVA".

Each photo then resolves to one of four buckets:

| Result | Meaning |
|---|---|
| **Matched** | Exact key hit on exactly one student — auto-assigned. |
| **Needs review** | Exact hit on several students, or a fuzzy match above the token-set threshold. Shows a thumbnail and a student dropdown. |
| **Unmatched** | No plausible candidate. |
| **Missing** | Students in the roster with no photo. |

Nothing exports until the reviewer has resolved the review bucket. Matching is
never 100%, so the review step is the safety net rather than an optional extra.

Implementation: [`photomatch.js`](../../tools/config-builder/src/photomatch.js)
(pure, unit-tested) and [`untar.js`](../../tools/config-builder/src/untar.js) for
reading the Moodle archive.

## Image handling

Moodle photos are already 100×100. The builder still **re-encodes** rather than
passing the bytes through: decode → draw to a 100×100 canvas → `toBlob` JPEG at
about q0.85. This guarantees a **baseline** JPEG (the P4 hardware decoder does
not handle progressive), strips EXIF orientation, and bounds the file size. The
cost is one light recompression.

A 100×100 baseline JPEG is roughly 3–8 KB, so a 600-student roster adds about
3–5 MB to the tar — well under the 16 MB cap.

## On-device display

Avatars render through the LVGL TJPGD decoder (`LV_USE_TJPGD`) over the
read-only SD `S:` driver (`src/ui/lvgl_fs_sd.cpp`). Every display site goes
through `student_photo_image(parent, id, max_px)`
(`src/ui/components/student_photo.cpp`), which fits the image into a square box
with its aspect ratio preserved — `AVATAR_MAX_PX` (250) on the check-in, enroll
and kiosk panels, 64 on the face-verify thumbnail. The fit **enlarges** as well
as shrinks, so today's 100×100 sources are upscaled and look correspondingly
soft; raising the builder's output size is the fix if that matters. Scale
arithmetic is the hardware-free `app/photo_fit`, which is native-tested.

A student with no avatar file gets a neutral placeholder circle.

## Deferred

- **Streaming unpack.** The importer buffers the whole tar in PSRAM rather than
  streaming it to a staging file. Fine at the 16 MB cap.
- **Device-side skip+warn** for avatars whose id matches no student. Harmless
  today — they simply never display.
- **Orphan cleanup.** Removing a student leaves their avatar on the card; an
  import overwrites but never deletes.

## Changelog

- **2026-07-30** — avatars are scaled to fit rather than drawn at their intrinsic
  size, which previously made a 100×100 photo a small thumbnail on a 480×800
  screen.
- **2026-07-28** — built end-to-end and verified on device: builder-side ingest
  (`untar.js`, `photomatch.js`, review UI), tar contract extended with
  `students/photos/<id>.jpg`, staged-tar cap raised 1 MB → 16 MB, and the device
  importer applying avatars while preserving `/photos/**` and
  `/students/checkins/**`.
- **2026-07-27** — device display: the check-in overlay reads
  `/students/photos/<id>.jpg` through TJPGD, with a placeholder fallback.
