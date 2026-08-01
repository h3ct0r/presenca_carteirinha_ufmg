#include "ui/components/student_photo.h"

// LVGL 9 keeps the decoder structs in a private header (the public API only
// forward-declares them), and avatar_decode() below has to read
// `dsc.header` / `dsc.decoded` to assemble the tiles. Pulled in here rather
// than switching LV_USE_PRIVATE_API on globally for one file.
#include <src/draw/lv_image_decoder_private.h>

#include <SD_MMC.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "app/photo_fit.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "services/roster_service.h"
#include "esp32-hal-log.h"
#include "storage/sd_card.h"

static const char* TAG = "student_photo";

static const char* PHOTO_DIR = "/students/photos";

// photo_fit stays free of lvgl.h so it can be host-tested; this is the one file
// that sees both constants, so it is where they get tied together.
static_assert(PHOTO_SCALE_UNIT == LV_SCALE_NONE,
              "photo_fit's scale unit must match LVGL's LV_SCALE_NONE");

// Two independent sanity limits, because a JPEG has two sizes that can bite.
//
// The FILE bound catches a long blocking SD read on the LVGL thread. Authored
// avatars are 100x100 baseline JPEGs (~3-8 KB; see
// docs/software/STUDENT_PHOTOS.md), so 64 KB is ~10x a legitimate one.
static constexpr size_t AVATAR_MAX_BYTES = 64 * 1024;

// The PIXEL bound catches what the file expands to. avatar_decode() holds the
// whole image at 2 bytes/px while it is on screen, and dimensions are free in a
// JPEG: a 2000x2000 photo compresses well under the file limit above and would
// still claim 8 MB of PSRAM to be drawn in a 250 px box. 512 is 2x the biggest
// box any caller asks for (AVATAR_MAX_PX), so anything past it is a mis-sized
// file rather than an avatar.
static constexpr int AVATAR_MAX_EDGE_PX = 512;

bool student_photo_exists(const char* student_id) {
    if (!student_id || !student_id[0] || !sd_card_mount()) return false;
    char path[80];
    snprintf(path, sizeof(path), "%s/%s.jpg", PHOTO_DIR, student_id);
    return SD_MMC.exists(path);
}

// "12345.jpg" -> "12345". False for anything that isn't a .jpg, or whose stem
// is longer than a student id can be (so it could never match one anyway).
static bool photo_stem(const char* filename, char* out, size_t cap) {
    if (!filename || !out || !cap) return false;
    const char* dot = strrchr(filename, '.');
    if (!dot || strcasecmp(dot, ".jpg") != 0) return false;
    size_t len = (size_t)(dot - filename);
    if (len == 0 || len >= cap) return false;
    memcpy(out, filename, len);
    out[len] = '\0';
    return true;
}

static bool class_has_student_id(const class_rec_t* cls, const char* id) {
    for (int j = 0; j < cls->roster_count; j++) {
        const student_t* st = roster_student_at(cls->roster[j]);
        if (st && strcmp(st->id, id) == 0) return true;
    }
    return false;
}

bool student_photo_src(const char* student_id, char* out, size_t cap) {
    if (!out || !cap) return false;
    out[0] = '\0';
    if (!student_photo_exists(student_id)) return false;

    char path[80];
    snprintf(path, sizeof(path), "/students/photos/%s.jpg", student_id);
    File f = SD_MMC.open(path, FILE_READ);
    if (!f) {
        ESP_LOGW(TAG, "%s exists but cannot be opened — using the placeholder", path);
        return false;
    }
    size_t sz = f.size();
    f.close();
    if (sz == 0 || sz > AVATAR_MAX_BYTES) {
        ESP_LOGE(TAG,
                 "%s is %u bytes (limit %u) — refusing to decode it; re-export the "
                 "avatars at 100x100 (see docs/software/STUDENT_PHOTOS.md). Using the "
                 "placeholder.",
                 path, (unsigned)sz, (unsigned)AVATAR_MAX_BYTES);
        return false;
    }

    snprintf(out, cap, "S:/students/photos/%s.jpg", student_id);  // LVGL drive-letter form
    return true;
}

// One pass over the photo directory, matching filenames against the roster in
// RAM. The obvious version — student_photo_exists() per student — walked that
// same flat directory once per student (it holds EVERY student's avatar, not
// just this class's), which is what made opening a class screen lag.
int student_photo_count_for_class(const class_rec_t* cls) {
    if (!cls || !sd_card_mount()) return 0;
    const int64_t t0 = esp_timer_get_time();

    File dir = SD_MMC.open(PHOTO_DIR);
    if (!dir || !dir.isDirectory()) {
        if (dir) dir.close();
        return 0;
    }

    int n = 0, seen = 0;
    File e;
    while ((e = dir.openNextFile())) {
        if (!e.isDirectory()) {
            seen++;
            char id[sizeof(student_t::id)];
            if (photo_stem(e.name(), id, sizeof(id)) && class_has_student_id(cls, id)) n++;
        }
        e.close();
    }
    dir.close();

    // Instrumentation: this is the one SD cost on the class-screen path, and it
    // can only be measured on the device.
    ESP_LOGI(TAG, "photo count for %s: %d/%d students, %d files scanned, %lld ms", cls->code, n,
             cls->roster_count, seen, (long long)((esp_timer_get_time() - t0) / 1000));
    return n;
}

// A whole decoded avatar, owned by the image widget it is attached to. The
// pixels follow this header in the same PSRAM block, so freeing is one call.
struct avatar_bitmap_t {
    lv_image_dsc_t dsc;
};

static void avatar_free_cb(lv_event_t* e) {
    heap_caps_free(lv_event_get_user_data(e));
}

// Decodes a JPEG into ONE RGB565 buffer and wraps it as an image descriptor.
//
// This exists because LVGL cannot scale a JPEG file directly. Its TJPGD decoder
// only implements `get_area`: it never produces a whole decoded image, it hands
// back one MCU tile at a time (8x8 or 16x16 px) and leaves `decoder_dsc->decoded`
// NULL at open. lv_draw_image then takes its "draw in smaller pieces" path,
// where every tile is drawn as an image of its own — while `scale` and `pivot`
// still describe the WHOLE image. Each tile therefore gets blown up 2.5x and
// stamped at unscaled coordinates, which is the block mosaic that showed up
// instead of an enlarged photo. Drawing 1:1 was fine, which is why this only
// appeared when avatars started being fitted to a box.
//
// Assembling the tiles here gives LVGL a plain in-memory image, the case its
// transform path does support (with antialiasing, since LV_COLOR_DEPTH is 16).
static avatar_bitmap_t* avatar_decode(const char* src) {
    lv_image_decoder_dsc_t dec;
    lv_image_decoder_args_t args;
    lv_memzero(&args, sizeof(args));
    if (lv_image_decoder_open(&dec, src, &args) != LV_RESULT_OK) return nullptr;

    const int w = (int)dec.header.w, h = (int)dec.header.h;
    // TJPGD always reports RGB888; anything else means another decoder answered
    // and this tile-assembly would misinterpret the rows.
    if (w <= 0 || h <= 0 || dec.header.cf != LV_COLOR_FORMAT_RGB888) {
        ESP_LOGW(TAG, "%s: %dx%d cf=%d — not a decodable RGB888 JPEG", src, w, h,
                 (int)dec.header.cf);
        lv_image_decoder_close(&dec);
        return nullptr;
    }
    // AVATAR_MAX_BYTES bounds the FILE; this bounds what it expands to. A photo
    // with huge dimensions can still compress under that limit, and at 2 bytes
    // per pixel a 2000x2000 one would claim 8 MB of PSRAM for a 250 px box.
    if (w > AVATAR_MAX_EDGE_PX || h > AVATAR_MAX_EDGE_PX) {
        ESP_LOGE(TAG,
                 "%s is %dx%d (limit %d per side) — refusing to decode it; re-export the "
                 "avatars (see docs/software/STUDENT_PHOTOS.md). Using the placeholder.",
                 src, w, h, AVATAR_MAX_EDGE_PX);
        lv_image_decoder_close(&dec);
        return nullptr;
    }

    // Stored as RGB565, the panel's own format: two thirds of the RGB888 size
    // with nothing lost (the display cannot show more), and it saves LVGL a
    // per-pixel 888->565 conversion on every redraw.
    const uint32_t stride = (uint32_t)w * 2;
    const size_t bytes = (size_t)stride * h;
    // One PSRAM block holds the descriptor and the pixels: the LVGL pool
    // (LV_MEM_SIZE, 512 KB, and a silent freeze when exhausted) never sees any
    // of this, and there is a single allocation to fail and a single to free.
    uint8_t* block = (uint8_t*)heap_caps_malloc(sizeof(avatar_bitmap_t) + bytes, MALLOC_CAP_SPIRAM);
    if (!block) {
        ESP_LOGE(TAG, "%s: out of PSRAM decoding %dx%d (%u B)", src, w, h, (unsigned)bytes);
        lv_image_decoder_close(&dec);
        return nullptr;
    }
    avatar_bitmap_t* bm = (avatar_bitmap_t*)block;
    uint8_t* pixels = block + sizeof(avatar_bitmap_t);

    // Tiles arrive left to right, top to bottom; `got` is both the cursor and
    // the answer, and must start as LV_COORD_MIN to mean "from the beginning".
    const lv_area_t full = {0, 0, (int32_t)w - 1, (int32_t)h - 1};
    lv_area_t got = {LV_COORD_MIN, LV_COORD_MIN, LV_COORD_MIN, LV_COORD_MIN};
    while (lv_image_decoder_get_area(&dec, &full, &got) == LV_RESULT_OK) {
        const lv_draw_buf_t* tile = dec.decoded;
        if (!tile || !tile->data) break;
        const int tw = lv_area_get_width(&got), th = lv_area_get_height(&got);
        for (int y = 0; y < th; y++) {
            const uint8_t* s = tile->data + (size_t)y * tile->header.stride;
            uint16_t* d = (uint16_t*)(pixels + (size_t)(got.y1 + y) * stride) + got.x1;
            // TJPGD emits B,G,R per pixel (tjpgd.c, JD_FORMAT 0) — the same byte
            // order LVGL's RGB888 uses — and lv_color16_t packs red in the high
            // bits, green in the middle, blue low.
            for (int x = 0; x < tw; x++, s += 3) {
                d[x] = (uint16_t)(((uint16_t)(s[2] & 0xF8) << 8) |
                                  ((uint16_t)(s[1] & 0xFC) << 3) | (uint16_t)(s[0] >> 3));
            }
        }
    }
    lv_image_decoder_close(&dec);

    lv_memzero(&bm->dsc, sizeof(bm->dsc));
    bm->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    bm->dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    bm->dsc.header.w = (uint32_t)w;
    bm->dsc.header.h = (uint32_t)h;
    bm->dsc.header.stride = stride;
    bm->dsc.data_size = (uint32_t)bytes;
    bm->dsc.data = pixels;
    return bm;
}

lv_obj_t* student_photo_image(lv_obj_t* parent, const char* student_id, int max_px) {
    char src[80];
    if (!parent || !student_photo_src(student_id, src, sizeof(src))) return nullptr;

    avatar_bitmap_t* bm = avatar_decode(src);
    if (!bm) return nullptr;  // caller draws its placeholder

    lv_obj_t* img = lv_image_create(parent);
    lv_image_set_src(img, &bm->dsc);
    lv_obj_add_event_cb(img, avatar_free_cb, LV_EVENT_DELETE, bm);

    const int sw = (int)bm->dsc.header.w;
    const int sh = (int)bm->dsc.header.h;

    lv_image_set_scale(img, (uint32_t)photo_fit_scale(sw, sh, max_px));

    // lv_image reports its UNTRANSFORMED size as its self-size, so without this
    // the layout would reserve the source's pixels (100x100) while painting the
    // scaled ones (250x250) — overlapping whatever sits below. Sizing the widget
    // to the drawn size also gives clip_corner the right box to round.
    int dw = 0, dh = 0;
    photo_fit_size(sw, sh, max_px, &dw, &dh);
    lv_obj_set_size(img, dw, dh);
    return img;
}
