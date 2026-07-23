#pragma once

#include "app/teacher.h"

// Access control against the loaded device config (config_service). Every
// check fails when no valid config is loaded, so the device stays locked
// until a valid config.json is present.

// Looks up the professor whose configured rfid_uid matches uid_hex. On a
// match, copies that teacher into *out (if non-NULL) and returns true.
// Comparison ignores case and separators (':', '-', ' ').
bool auth_lookup_uid(const char* uid_hex, teacher_t* out);

// Looks up the professor whose password matches exactly. Passwords are unique
// per professor (enforced at config load), so a match identifies exactly one.
// Copies that teacher into *out (if non-NULL) and returns true. An empty
// password never matches.
bool auth_lookup_password(const char* password, teacher_t* out);
