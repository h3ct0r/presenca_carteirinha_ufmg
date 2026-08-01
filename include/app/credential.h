#pragma once

#include <stddef.h>
#include <stdint.h>

// Turns the two secrets the device stores on the SD card — RFID card UIDs and
// professor passwords — into keyed fingerprints, so `students.json` and
// `config.json` carry nothing that can be replayed onto a blank card or typed
// into the keypad.
//
// Both are HMAC-SHA256 under a per-device key that lives in NVS and never
// touches the card (services/device_secret.h). The key is what makes this work
// at all: a card UID is 4-10 low-entropy bytes and a password is a handful of
// digits, so an UNKEYED hash of either is exhausted in seconds. An attacker who
// reads the card gets the fingerprints but not the key, and so has nothing to
// search against.
//
// Not a slow KDF. PBKDF2 exists for the case where the attacker holds the hash
// *and* its salt; here the key never leaves the device, so iteration count would
// buy nothing that the key does not already buy. The stored form is version-
// prefixed so that judgement can be revisited without another schema change —
// see CREDENTIAL_PREFIX.
//
// Pure computation, key passed in: app/ must not reach into services/.

// Version tag on every stored fingerprint. Bump it (and add a branch at the
// comparison sites) if the construction ever changes.
#define CREDENTIAL_PREFIX "v1:"

// Truncated to 80 bits (20 hex chars), sized so that the whole stored string —
// "v1:" + hex + NUL — is exactly 24 bytes and fits student_t::rfid_uid[24]
// unchanged. That keeps the 600-entry registry at its current 55,200 B in the
// constrained internal pool (docs/software/ARCHITECTURE.md §Memory budget); a
// full 256-bit hex would have added 26 KB.
//
// 80 bits is not a security parameter here — the HMAC key is what resists
// guessing — it only has to avoid accidental collisions. Among 600 cards the
// birthday probability is ~1e-19.
constexpr size_t UID_FINGERPRINT_HEX = 20;
// "v1:" + 20 hex + NUL == 24.
constexpr size_t UID_FINGERPRINT_CAP = 3 + UID_FINGERPRINT_HEX + 1;

// Full 256 bits for passwords: only CONFIG_MAX_TEACHERS of them, so the width
// costs nothing worth saving.
constexpr size_t PASSWORD_HASH_HEX = 64;
constexpr size_t PASSWORD_HASH_CAP = 3 + PASSWORD_HASH_HEX + 1;

// Fingerprints a scanned or stored card UID. The UID is canonicalized first
// (uid_normalize: separators dropped, upper-cased), so "e0:d1-33 5f" and
// "E0D1335F" produce the same fingerprint. Writes "" when the UID has no usable
// characters or `cap` is too small. Output: "v1:<23 hex>".
void uid_fingerprint(const uint8_t* key, size_t key_len, const char* uid, char* out,
                     size_t cap);

// Hashes a professor password verbatim (no canonicalization — a password's
// bytes are its own). Writes "" for an empty password, which is how "no password
// set" is represented. Output: "v1:<64 hex>".
void password_hash(const uint8_t* key, size_t key_len, const char* password, char* out,
                   size_t cap);

// True if `s` looks like a value this module produced, rather than the cleartext
// an authoring tool wrote. Used to tell an already-converted config.json from
// one that still needs converting, and to reject a plaintext rfid_uid outright.
bool credential_is_fingerprint(const char* s, size_t hex_len);
