#pragma once

#include <stddef.h>
#include <stdint.h>

// Reader for the uncompressed POSIX ustar archive the config-builder emits
// (tools/config-builder/src/tarball.js). Pure and hardware-free: it walks a
// whole archive already in memory and hands each regular-file entry to a
// visitor. The device importer stages the tar on SD, reads it into a buffer,
// and drives this. See docs/software/CONFIG_IMPORT.md §4 for the archive format
// and the name whitelist enforced here.

typedef enum {
    USTAR_OK = 0,         // reached the end-of-archive marker cleanly
    USTAR_ERR_TRUNCATED,  // ran out of bytes mid-header or mid-payload
    USTAR_ERR_MAGIC,      // a header lacked the "ustar" magic
    USTAR_ERR_CHECKSUM,   // a header's stored checksum didn't match
    USTAR_ERR_NAME,       // an entry name failed the §4 path / whitelist rules
    USTAR_ERR_TOO_BIG,    // the archive exceeds max_bytes
    USTAR_ERR_ABORT,      // the visitor returned false
} ustar_status_t;

typedef struct {
    const char* name;     // NUL-terminated (bounded copy, valid for the call)
    const uint8_t* data;  // payload, points into the caller's buffer
    size_t size;          // payload length in bytes
} ustar_entry_t;

// Return true to keep iterating, false to stop (-> USTAR_ERR_ABORT).
typedef bool (*ustar_visitor_t)(const ustar_entry_t* entry, void* ctx);

// Walk every regular-file entry in the archive [buf, buf+len). Enforces the §4
// rules on every entry BEFORE the visitor sees it — any leading '/', a '..'
// path component, a backslash / ':' , or a regular-file name outside
//   { "config.json", "students/students.json", "classes/<seg>/class.json" }
// fails the whole archive with USTAR_ERR_NAME. Directory entries (typeflag '5'
// or a trailing '/') are path-checked and skipped, never handed to the visitor.
// `max_bytes` caps the archive (0 = no cap) and yields USTAR_ERR_TOO_BIG.
// `visit` may be NULL to validate structure + names without visiting.
ustar_status_t ustar_iterate(const uint8_t* buf, size_t len, size_t max_bytes,
                             ustar_visitor_t visit, void* ctx);

// The §4 name predicate for a REGULAR FILE entry, exposed for reuse and tests.
// True only for the three authored paths, with the path-safety checks applied.
bool ustar_name_allowed(const char* name);
