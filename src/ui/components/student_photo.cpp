#include "ui/components/student_photo.h"

#include <SD_MMC.h>
#include <stdio.h>

#include "app/photo_fit.h"
#include "esp32-hal-log.h"
#include "storage/sd_card.h"

static const char* TAG = "student_photo";

// photo_fit stays free of lvgl.h so it can be host-tested; this is the one file
// that sees both constants, so it is where they get tied together.
static_assert(PHOTO_SCALE_UNIT == LV_SCALE_NONE,
              "photo_fit's scale unit must match LVGL's LV_SCALE_NONE");

// A sanity limit on the FILE, not on the decode: LVGL 9.5's TJPGD decoder
// streams a JPEG MCU-by-MCU through a ~4 KB work buffer, so pixel dimensions do
// not translate into a full-frame allocation out of the LVGL heap
// (LV_MEM_SIZE, 256 KB). What a wildly oversized file does buy is a long
// blocking read plus a long decode on the LVGL thread, on every redraw — the
// image cache is off (LV_CACHE_DEF_SIZE 0).
//
// Authored avatars are 100x100 baseline JPEGs (~3-8 KB; see
// docs/software/STUDENT_PHOTOS.md).
// 64 KB is ~10x that, so anything above it is a mis-sized file rather than a
// legitimate avatar: skip it and say so.
static constexpr size_t AVATAR_MAX_BYTES = 64 * 1024;

bool student_photo_exists(const char* student_id) {
    if (!student_id || !student_id[0] || !sd_card_mount()) return false;
    char path[80];
    snprintf(path, sizeof(path), "/students/photos/%s.jpg", student_id);
    return SD_MMC.exists(path);
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

lv_obj_t* student_photo_image(lv_obj_t* parent, const char* student_id, int max_px) {
    char src[80];
    if (!parent || !student_photo_src(student_id, src, sizeof(src))) return nullptr;

    lv_obj_t* img = lv_image_create(parent);
    lv_image_set_src(img, src);  // parses the JPEG header, caching w/h on the widget

    const int sw = lv_image_get_src_width(img);
    const int sh = lv_image_get_src_height(img);
    if (sw <= 0 || sh <= 0) {
        // The file passed the size check but the decoder could not read its
        // header. Drop the widget so the caller falls back to the placeholder
        // rather than leaving an invisible 0x0 image in the layout.
        ESP_LOGW(TAG, "%s: no usable JPEG header — using the placeholder", src);
        lv_obj_delete(img);
        return nullptr;
    }

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
