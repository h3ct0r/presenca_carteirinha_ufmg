#include "services/export_service.h"

#include <FS.h>
#include <SD_MMC.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "services/roster_service.h"
#include "storage/attendance_store.h"

// Cap on session dates read per class (a class won't hold more than a school
// year of daily sessions). The date list is heap-allocated to keep it off the
// (shallow, LVGL-thread) stack.
static constexpr int MAX_DAYS = 366;

void export_path_for(const class_rec_t* cls, char* out, size_t cap) {
    snprintf(out, cap, EXPORT_DIR "/%s.csv", cls ? cls->code : "");
}

bool export_exists(const class_rec_t* cls) {
    if (!cls) return false;
    char path[80];
    export_path_for(cls, path, sizeof(path));
    return SD_MMC.exists(path);
}

export_metrics_t export_metrics(const class_rec_t* cls) {
    export_metrics_t m = {};
    if (!cls) return m;
    m.student_count = cls->roster_count;

    char(*dates)[12] = (char(*)[12])malloc(MAX_DAYS * 12);
    if (!dates) return m;
    int n = attendance_list_dates(cls->dir, dates, MAX_DAYS);
    m.day_count = n;
    if (n > 0) {
        // attendance_list_dates returns newest-first.
        snprintf(m.end_date, sizeof(m.end_date), "%s", dates[0]);
        snprintf(m.start_date, sizeof(m.start_date), "%s", dates[n - 1]);
    }
    free(dates);
    return m;
}

// Snapshot/restore of the single global attendance session, so exporting (which
// opens each historical date to tally absences) leaves any in-progress roll
// call untouched.
namespace {
struct SessionSnapshot {
    bool open;
    char dir[24];
    char date[12];
};

SessionSnapshot snapshot_session(void) {
    SessionSnapshot s = {};
    s.open = attendance_is_open();
    snprintf(s.dir, sizeof(s.dir), "%s", attendance_dir());
    snprintf(s.date, sizeof(s.date), "%s", attendance_date());
    return s;
}

void restore_session(const SessionSnapshot& s) {
    if (s.open) {
        attendance_open(s.dir, s.date);
    } else {
        attendance_close();
    }
}
}  // namespace

export_result_t export_write_csv(const class_rec_t* cls, progress_cb_t cb, void* ctx) {
    export_result_t r = {};
    if (!cls) {
        snprintf(r.message, sizeof(r.message), "No class");
        return r;
    }
    export_path_for(cls, r.path, sizeof(r.path));

    char(*dates)[12] = (char(*)[12])malloc(MAX_DAYS * 12);
    if (!dates) {
        r.path[0] = '\0';
        snprintf(r.message, sizeof(r.message), "Out of memory");
        return r;
    }
    int days = attendance_list_dates(cls->dir, dates, MAX_DAYS);

    // FREQ = absent days per student. Fold every date once (dates outer,
    // students inner), counting a student absent whenever they aren't present.
    int total = cls->roster_count;
    int* freq = (int*)calloc(total > 0 ? total : 1, sizeof(int));
    if (!freq) {
        free(dates);
        r.path[0] = '\0';
        snprintf(r.message, sizeof(r.message), "Out of memory");
        return r;
    }

    SessionSnapshot saved = snapshot_session();
    for (int d = 0; d < days; d++) {
        attendance_open(cls->dir, dates[d]);
        for (int j = 0; j < total; j++) {
            const student_t* st = roster_student_at(cls->roster[j]);
            if (st && !attendance_is_present(st->id)) freq[j]++;
        }
        // Each iteration re-reads a whole session file, so this is where the
        // time goes and where the UI needs to see movement.
        if (cb) {
            progress_t p = {"Reading sessions", dates[d], d + 1, days};
            cb(&p, ctx);
        }
    }
    restore_session(saved);
    free(dates);

    // Build the CSV in a heap buffer (one write), then persist it.
    size_t cap = 32 + (size_t)(total + 1) * 40;
    char* buf = (char*)malloc(cap);
    if (!buf) {
        free(freq);
        r.path[0] = '\0';
        snprintf(r.message, sizeof(r.message), "Out of memory");
        return r;
    }
    size_t len = 0;
    len += snprintf(buf + len, cap - len, "MATRICULA,FREQ\n");
    for (int j = 0; j < total; j++) {
        const student_t* st = roster_student_at(cls->roster[j]);
        if (!st) continue;
        len += snprintf(buf + len, cap - len, "%s,%d\n", st->id, freq[j]);
    }
    free(freq);

    if (!SD_MMC.exists(EXPORT_DIR)) SD_MMC.mkdir(EXPORT_DIR);
    File f = SD_MMC.open(r.path, FILE_WRITE, true);
    if (!f) {
        free(buf);
        r.path[0] = '\0';
        snprintf(r.message, sizeof(r.message), "Could not write to SD card");
        return r;
    }
    size_t written = f.write((const uint8_t*)buf, len);
    f.close();
    free(buf);

    if (written != len) {
        r.path[0] = '\0';
        snprintf(r.message, sizeof(r.message), "Write incomplete");
        return r;
    }
    r.ok = true;
    r.size_bytes = (uint32_t)written;
    snprintf(r.message, sizeof(r.message), "Exported");
    return r;
}
