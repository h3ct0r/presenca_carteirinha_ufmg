#include "ui/lvgl_fs_sd.h"

#include <FS.h>
#include <SD_MMC.h>

#include "lvgl.h"
#include "storage/sd_card.h"

// LVGL passes the path with the drive letter already stripped (e.g. it hands
// "/students/photos/x.jpg" for "S:/students/photos/x.jpg"), which maps directly
// onto SD_MMC paths. Read-only: these are image assets, never written here.

static void* fs_open(lv_fs_drv_t*, const char* path, lv_fs_mode_t mode) {
    if (mode != LV_FS_MODE_RD) return nullptr;
    if (!sd_card_mount()) return nullptr;
    File f = SD_MMC.open(path, FILE_READ);
    if (!f || f.isDirectory()) {
        if (f) f.close();
        return nullptr;
    }
    return new File(f);  // freed in fs_close
}

static lv_fs_res_t fs_close(lv_fs_drv_t*, void* file_p) {
    File* f = (File*)file_p;
    f->close();
    delete f;
    return LV_FS_RES_OK;
}

static lv_fs_res_t fs_read(lv_fs_drv_t*, void* file_p, void* buf, uint32_t btr, uint32_t* br) {
    int n = ((File*)file_p)->read((uint8_t*)buf, btr);
    if (n < 0) {
        *br = 0;
        return LV_FS_RES_UNKNOWN;
    }
    *br = (uint32_t)n;
    return LV_FS_RES_OK;
}

static lv_fs_res_t fs_seek(lv_fs_drv_t*, void* file_p, uint32_t pos, lv_fs_whence_t whence) {
    File* f = (File*)file_p;
    uint32_t base = 0;
    if (whence == LV_FS_SEEK_CUR) base = f->position();
    else if (whence == LV_FS_SEEK_END) base = f->size();
    return f->seek(base + pos) ? LV_FS_RES_OK : LV_FS_RES_UNKNOWN;
}

static lv_fs_res_t fs_tell(lv_fs_drv_t*, void* file_p, uint32_t* pos_p) {
    *pos_p = ((File*)file_p)->position();
    return LV_FS_RES_OK;
}

void lvgl_fs_sd_init(void) {
    static lv_fs_drv_t drv;  // must outlive registration
    lv_fs_drv_init(&drv);
    drv.letter = 'S';
    drv.cache_size = 0;
    drv.open_cb = fs_open;
    drv.close_cb = fs_close;
    drv.read_cb = fs_read;
    drv.seek_cb = fs_seek;
    drv.tell_cb = fs_tell;
    lv_fs_drv_register(&drv);
}
