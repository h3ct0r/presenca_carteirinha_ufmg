#pragma once

#include <stddef.h>

#include "app/roster.h"

// Read-only summaries of a class roster, shared by the class screen and the
// class statistics screen so the two can never disagree. Pure logic — no LVGL,
// no SD — so it is covered by the native tests.

// Distinct turmas tracked in a breakdown. A class is at most
// ROSTER_MAX_CLASS_STUDENTS students, so in theory every one could carry its own
// turma, but a real class has a handful. Counting only the first few keeps the
// working set off the LVGL thread's stack (the full-size table cost ~800 bytes);
// anything past this is summarised as "+N more".
constexpr int CLASS_STATS_MAX_TURMAS = 16;

// Formats the class's turma breakdown into `out`, e.g.
// "TE1: 12    TE2: 8    (none): 3". Untagged students group under "(none)".
//
// Returns true when at least one student carries a turma tag, so the caller can
// hide the line entirely for a class that does not use them. `out` is always
// NUL-terminated (empty when there is nothing to report), and the text is
// truncated rather than overrun if `cap` is too small.
//
// ASCII only by design — the device fonts cover 0x20-0x7F (see
// docs/software/BACKLOG.md).
bool class_turma_breakdown(const class_rec_t* cls, char* out, size_t cap);
