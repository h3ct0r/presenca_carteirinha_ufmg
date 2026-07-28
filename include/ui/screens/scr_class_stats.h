#pragma once

#include "ui/screen.h"

// Staff: one class's statistics (roster, photo coverage, turma breakdown,
// attendance rate) and its editable per-class settings (name, schedule, color,
// photo-capture override). Opened from the gear button on a class card.
//
// on_show arg: const class_rec_t* from roster_service. Pass NULL to re-show the
// last class.
extern const screen_t scr_class_stats;
