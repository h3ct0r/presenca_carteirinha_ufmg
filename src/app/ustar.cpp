#include "app/ustar.h"

#include <string.h>

// ustar header field offsets (POSIX). We read only what the contract needs;
// mode/uid/gid/mtime are ignored (but still counted by the checksum).
static constexpr size_t OFF_NAME = 0;      // 100 bytes
static constexpr size_t OFF_SIZE = 124;    // 12 bytes, octal
static constexpr size_t OFF_CHKSUM = 148;  // 8 bytes, octal (field itself = spaces during calc)
static constexpr size_t OFF_TYPEFLAG = 156;
static constexpr size_t OFF_MAGIC = 257;  // "ustar"
static constexpr size_t BLOCK = 512;

// Parse up to n bytes of a NUL/space-terminated octal field.
static unsigned long parse_octal(const uint8_t* p, size_t n) {
    unsigned long v = 0;
    size_t i = 0;
    while (i < n && p[i] == ' ') i++;  // leading spaces
    for (; i < n; i++) {
        uint8_t c = p[i];
        if (c < '0' || c > '7') break;  // stops at NUL, trailing space, or garbage
        v = (v << 3) + (unsigned long)(c - '0');
    }
    return v;
}

static bool has_magic(const uint8_t* h) { return memcmp(h + OFF_MAGIC, "ustar", 5) == 0; }

// Sum of all 512 header bytes with the 8-byte checksum field taken as spaces,
// compared to the stored octal value (unsigned, as the writer computes it).
static bool checksum_ok(const uint8_t* h) {
    unsigned long stored = parse_octal(h + OFF_CHKSUM, 8);
    unsigned long sum = 0;
    for (size_t i = 0; i < BLOCK; i++) {
        sum += (i >= OFF_CHKSUM && i < OFF_CHKSUM + 8) ? 0x20 : h[i];
    }
    return sum == stored;
}

// Path safety, applied to EVERY entry (files and directories): no absolute
// path, no backslash / drive prefix, no ".." path component.
static bool path_is_safe(const char* n) {
    if (!n || n[0] == '\0' || n[0] == '/') return false;
    for (const char* c = n; *c; c++) {
        if (*c == '\\' || *c == ':') return false;
    }
    for (const char* s = n;;) {
        const char* slash = strchr(s, '/');
        size_t seg = slash ? (size_t)(slash - s) : strlen(s);
        if (seg == 2 && s[0] == '.' && s[1] == '.') return false;
        if (!slash) break;
        s = slash + 1;
    }
    return true;
}

// True when `rest` is "<seg>/<suffix>" with exactly one non-empty segment and
// no further '/' before the fixed suffix (e.g. "<CODE>/class.json").
static bool one_segment_then(const char* rest, const char* suffix) {
    const char* slash = strchr(rest, '/');
    if (!slash || slash == rest) return false;  // missing or empty segment
    return strcmp(slash, suffix) == 0;          // exact suffix, no further '/'
}

// True for "<pfx><seg>.jpg" with a single non-empty <seg> (no '/'). Used for the
// authored avatar tree students/photos/<id>.jpg.
static bool single_seg_file(const char* n, const char* pfx, const char* ext) {
    size_t pl = strlen(pfx);
    if (strncmp(n, pfx, pl) != 0) return false;
    const char* seg = n + pl;                              // segment after the prefix
    if (seg[0] == '\0' || strchr(seg, '/')) return false;  // empty or nested
    size_t sl = strlen(seg), el = strlen(ext);
    return sl > el && strcmp(seg + sl - el, ext) == 0;  // ends with ext, has a stem
}

// The authored file paths, and nothing else. Assumes path_is_safe passed.
static bool is_whitelisted_file(const char* n) {
    if (strcmp(n, "config.json") == 0) return true;
    if (strcmp(n, "students/students.json") == 0) return true;
    // classes/<seg>/class.json — exactly one non-empty segment, no nested '/'.
    if (strncmp(n, "classes/", 8) == 0) return one_segment_then(n + 8, "/class.json");
    // students/photos/<id>.jpg — one non-empty segment ending in ".jpg".
    if (strncmp(n, "students/photos/", 16) == 0)
        return single_seg_file(n, "students/photos/", ".jpg");
    return false;
}

bool ustar_name_allowed(const char* name) {
    return path_is_safe(name) && is_whitelisted_file(name);
}

ustar_status_t ustar_iterate(const uint8_t* buf, size_t len, size_t max_bytes,
                             ustar_visitor_t visit, void* ctx) {
    if (max_bytes && len > max_bytes) return USTAR_ERR_TOO_BIG;

    for (size_t off = 0;;) {
        if (off + BLOCK > len) return USTAR_ERR_TRUNCATED;  // no room for a header
        const uint8_t* h = buf + off;

        if (h[OFF_NAME] == 0) return USTAR_OK;  // empty name = end-of-archive marker
        if (!has_magic(h)) return USTAR_ERR_MAGIC;
        if (!checksum_ok(h)) return USTAR_ERR_CHECKSUM;

        char name[101];
        size_t k = 0;
        for (; k < 100 && h[OFF_NAME + k]; k++) name[k] = (char)h[OFF_NAME + k];
        name[k] = '\0';

        size_t data_off = off + BLOCK;
        unsigned long size = parse_octal(h + OFF_SIZE, 12);
        if (size > len - data_off) return USTAR_ERR_TRUNCATED;  // payload doesn't fit
        size_t padded = ((size + BLOCK - 1) / BLOCK) * BLOCK;

        if (!path_is_safe(name)) return USTAR_ERR_NAME;

        bool is_dir = (h[OFF_TYPEFLAG] == '5') || (k > 0 && name[k - 1] == '/');
        if (!is_dir) {
            if (!is_whitelisted_file(name)) return USTAR_ERR_NAME;
            ustar_entry_t e = {name, buf + data_off, (size_t)size};
            if (visit && !visit(&e, ctx)) return USTAR_ERR_ABORT;
        }

        off = data_off + padded;
    }
}
