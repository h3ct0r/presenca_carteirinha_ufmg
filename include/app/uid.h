#pragma once

#include <stddef.h>

// Canonicalizes an RFID UID string: strips ':', '-' and spaces and
// upper-cases, so formatting differences between config files, the reader
// and the student registry never cause false mismatches.
void uid_normalize(const char* in, char* out, size_t cap);
