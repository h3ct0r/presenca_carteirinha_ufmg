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
and kiosk panels, 64 on the face-verify thumbnail. Scale arithmetic is the
hardware-free `app/photo_fit`, which is native-tested.

**A JPEG file cannot be handed to LVGL and scaled.** TJPGD implements only the
decoder's `get_area` callback: it never produces a whole decoded image, it
returns one MCU tile (8×8 or 16×16 px) at a time and leaves `decoded` NULL at
open. `lv_draw_image` then takes its "draw in smaller pieces" path, which passes
each tile to the draw core *with the whole image's `scale` and `pivot`* — so
every tile is enlarged and stamped at unscaled coordinates. Drawing 1:1 is
unaffected, which is why this only appeared when avatars started being fitted to
a box, and why it looked like a mosaic of blocks rather than a big photo.

So `student_photo_image()` decodes the file into **one RGB565 buffer in PSRAM**
(`avatar_decode()`, 20 KB for a 100×100 avatar — `w × h × 2`, plus a 28-byte
descriptor in the same block) and sets that in-memory image as the source.
`lv_bin_decoder` reports an in-memory image as fully decoded, which is the case
LVGL's transform path supports — with antialiasing, since `LV_COLOR_DEPTH` is 16.
RGB565 is the panel's own format, so nothing is lost against the decoder's RGB888
and LVGL skips a per-pixel conversion on every redraw. The buffer is owned by the
widget and freed on `LV_EVENT_DELETE`; nothing lands in the LVGL pool. A side
benefit: the image cache is off (`LV_CACHE_DEF_SIZE 0`), so the old path
re-decoded from SD on *every* redraw; now it decodes once.

Two limits guard it, because a JPEG has two sizes: `AVATAR_MAX_BYTES` (64 KB)
bounds the file, and `AVATAR_MAX_EDGE_PX` (512) bounds what it expands to — a
photo with large dimensions can still compress under the file limit and would
otherwise claim megabytes of PSRAM for a 250 px box. Either limit falls back to
the placeholder with a log line.

The fit still **enlarges** as well as shrinks, so today's 100×100 sources are
interpolated up to 250 px and look correspondingly soft — that is the source
resolution, not the scaler. Raising the builder's output size is the fix if that
matters.

A student with no avatar file gets a neutral placeholder circle.

## Deferred

- **Streaming unpack.** The importer buffers the whole tar in PSRAM rather than
  streaming it to a staging file. Fine at the 16 MB cap.
- **Device-side skip+warn** for avatars whose id matches no student. Harmless
  today — they simply never display.
- **Orphan cleanup.** Removing a student leaves their avatar on the card; an
  import overwrites but never deletes.

## Changelog

- **2026-07-31** — avatars are decoded into a single RGB565 PSRAM buffer before display.
  Scaling a JPEG *file* never worked: LVGL's TJPGD decoder only streams MCU
  tiles, and the scaled draw applied the whole-image transform to each tile, so
  the 250 px avatar came out as a block mosaic (see §On-device display).
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
