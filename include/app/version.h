#pragma once

// Single source of truth for the firmware version — shown on the About screen
// and printed at boot. Header-only and hardware-free, so native tests can use it.
//
// Bump APP_VERSION by hand for a release. APP_GIT_SHA is injected at build time
// by tools/build/version_flags.py (a PlatformIO pre-script); it falls back to
// "nogit" when git is unavailable or the tree isn't a repository.
//
// Deliberately NOT written into the CSV export: "/csv_export/<code>.csv" is a
// fixed MATRICULA,FREQ contract (docs/software/EXPORT.md) consumed by an
// external grade-filling tool, so its header must not gain extra lines.

#define APP_VERSION "v0.2.0"

#ifndef APP_GIT_SHA
#define APP_GIT_SHA "nogit"
#endif

// Full build identity, e.g. "v0.2.0+89f20c1" — use this when displaying or
// logging, so a bug report pins the exact build.
#define APP_VERSION_FULL APP_VERSION "+" APP_GIT_SHA
