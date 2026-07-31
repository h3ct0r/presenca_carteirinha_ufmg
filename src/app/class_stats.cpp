#include "app/class_stats.h"

#include <stdio.h>
#include <string.h>

static const char* UNTAGGED = "(none)";

bool class_turma_breakdown(const class_rec_t* cls, char* out, size_t cap) {
    if (!out || cap == 0) return false;
    out[0] = '\0';
    if (!cls) return false;

    // Pointers into cls->roster_turma (or the literal above) — no copying, and
    // the table is small enough to sit on the stack comfortably.
    struct {
        const char* name;
        int count;
    } groups[CLASS_STATS_MAX_TURMAS];
    int ngroups = 0;
    int overflow = 0;  // students whose turma did not fit in the table
    bool any_tagged = false;

    for (int j = 0; j < cls->roster_count; j++) {
        const bool tagged = cls->roster_turma[j][0] != '\0';
        const char* name = tagged ? cls->roster_turma[j] : UNTAGGED;
        if (tagged) any_tagged = true;

        int g = -1;
        for (int k = 0; k < ngroups; k++) {
            if (strcmp(groups[k].name, name) == 0) {
                g = k;
                break;
            }
        }
        if (g >= 0) {
            groups[g].count++;
        } else if (ngroups < CLASS_STATS_MAX_TURMAS) {
            groups[ngroups].name = name;
            groups[ngroups].count = 1;
            ngroups++;
        } else {
            overflow++;  // too many distinct turmas to list them all
        }
    }

    size_t len = 0;
    for (int k = 0; k < ngroups; k++) {
        int w = snprintf(out + len, cap - len, "%s%s: %d", k ? "    " : "", groups[k].name,
                         groups[k].count);
        // Truncated (or failed): stop here rather than leaving a half-written
        // entry and advancing past the end of the buffer.
        if (w < 0 || (size_t)w >= cap - len) return any_tagged;
        len += (size_t)w;
    }
    if (overflow > 0) {
        int w = snprintf(out + len, cap - len, "    +%d more", overflow);
        if (w > 0 && (size_t)w < cap - len) len += (size_t)w;
    }
    return any_tagged;
}
