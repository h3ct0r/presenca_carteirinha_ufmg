#include "storage/checkin_store.h"

#include <FS.h>
#include <SD_MMC.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "storage/sd_card.h"

bool checkin_store_next_path(const char* student_id, const char* date, const char* class_code,
                             char* out, size_t cap) {
    if (!student_id || !student_id[0] || !out || cap == 0) return false;
    if (!sd_card_mount()) return false;

    SD_MMC.mkdir("/students");
    SD_MMC.mkdir("/students/checkins");
    char dir[80];
    snprintf(dir, sizeof(dir), "/students/checkins/%s", student_id);
    SD_MMC.mkdir(dir);

    // Files for this student are named "<date>_<code>_<NN>.jpg"; find the highest
    // NN already used for this date+class prefix.
    char prefix[64];
    snprintf(prefix, sizeof(prefix), "%s_%s_", date ? date : "", class_code ? class_code : "");
    size_t plen = strlen(prefix);

    int max_n = 0;
    File d = SD_MMC.open(dir);
    if (d && d.isDirectory()) {
        File e;
        while ((e = d.openNextFile())) {
            const char* name = e.name();  // basename on this core (see photo_store)
            if (strncmp(name, prefix, plen) == 0) {
                int n = atoi(name + plen);  // "<NN>.jpg" -> NN
                if (n > max_n) max_n = n;
            }
            e.close();
        }
        d.close();
    }

    snprintf(out, cap, "%s/%s%02d.jpg", dir, prefix, max_n + 1);
    return true;
}
