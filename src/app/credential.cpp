#include "app/credential.h"

#include <string.h>

#include "app/sha256.h"
#include "app/uid.h"

namespace {

// Shared shape: HMAC the text under the device key, then emit
// CREDENTIAL_PREFIX + the first `hex_len` hex characters of the digest.
void fingerprint(const uint8_t* key, size_t key_len, const char* text, size_t hex_len,
                 char* out, size_t cap) {
    if (!out || !cap) return;
    out[0] = '\0';
    if (!text || !text[0]) return;
    // Refuse rather than truncate: a short fingerprint would compare unequal to
    // the stored one and read as "wrong card", which is impossible to diagnose.
    const size_t need = strlen(CREDENTIAL_PREFIX) + hex_len + 1;
    if (cap < need) return;

    uint8_t mac[SHA256_DIGEST_LEN];
    hmac_sha256(key, key_len, text, strlen(text), mac);

    char hex[SHA256_DIGEST_LEN * 2 + 1];
    sha256_to_hex(mac, SHA256_DIGEST_LEN, hex, sizeof(hex));

    memcpy(out, CREDENTIAL_PREFIX, strlen(CREDENTIAL_PREFIX));
    memcpy(out + strlen(CREDENTIAL_PREFIX), hex, hex_len);
    out[strlen(CREDENTIAL_PREFIX) + hex_len] = '\0';

    memset(mac, 0, sizeof(mac));
    memset(hex, 0, sizeof(hex));
}

}  // namespace

void uid_fingerprint(const uint8_t* key, size_t key_len, const char* uid, char* out,
                     size_t cap) {
    if (!out || !cap) return;
    out[0] = '\0';
    // Canonicalize first: the same physical card reads back with different
    // separators and case depending on where the string came from, and those
    // must not produce different fingerprints.
    char norm[48];
    uid_normalize(uid ? uid : "", norm, sizeof(norm));
    fingerprint(key, key_len, norm, UID_FINGERPRINT_HEX, out, cap);
}

void password_hash(const uint8_t* key, size_t key_len, const char* password, char* out,
                   size_t cap) {
    fingerprint(key, key_len, password, PASSWORD_HASH_HEX, out, cap);
}

bool credential_is_fingerprint(const char* s, size_t hex_len) {
    if (!s) return false;
    const size_t plen = strlen(CREDENTIAL_PREFIX);
    if (strncmp(s, CREDENTIAL_PREFIX, plen) != 0) return false;
    if (strlen(s) != plen + hex_len) return false;
    for (const char* c = s + plen; *c; c++) {
        const bool hex = (*c >= '0' && *c <= '9') || (*c >= 'a' && *c <= 'f');
        if (!hex) return false;
    }
    return true;
}
