#include "ui/components/student_photo.h"

#include <SD_MMC.h>
#include <stdio.h>

#include "storage/sd_card.h"

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
    snprintf(out, cap, "S:/students/photos/%s.jpg", student_id);  // LVGL drive-letter form
    return true;
}
