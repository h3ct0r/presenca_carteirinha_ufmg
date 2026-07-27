#pragma once

// Registers a read-only LVGL filesystem driver on drive letter 'S', backed by
// the SD card (SD_MMC). It lets LVGL load image assets straight off the card by
// path, e.g. lv_image_set_src(img, "S:/students/photos/2023-0142.jpg"), with the
// TJPGD decoder (LV_USE_TJPGD) handling baseline JPEGs.
//
// Call once from ui_init(), after LVGL is initialized. LVGL thread only.
void lvgl_fs_sd_init(void);
