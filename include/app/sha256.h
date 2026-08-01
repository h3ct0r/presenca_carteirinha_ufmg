#pragma once

#include <stddef.h>
#include <stdint.h>

// SHA-256 and HMAC-SHA256, vendored rather than taken from mbedtls because the
// native test env has no ESP-IDF: everything that hashes has to compile and run
// on the host too, and this is the one place where a silent implementation bug
// would be both invisible and catastrophic. test/native/test_crypto checks it
// against the published RFC/NIST vectors.
//
// Pure computation, no hardware — hence app/ (see ARCHITECTURE.md §Layers).

constexpr size_t SHA256_DIGEST_LEN = 32;
constexpr size_t SHA256_BLOCK_LEN = 64;

typedef struct {
    uint32_t state[8];
    uint64_t bitlen;
    uint8_t buf[SHA256_BLOCK_LEN];
    size_t buflen;
} sha256_ctx_t;

void sha256_init(sha256_ctx_t* c);
void sha256_update(sha256_ctx_t* c, const void* data, size_t len);
void sha256_final(sha256_ctx_t* c, uint8_t out[SHA256_DIGEST_LEN]);

// One-shot convenience.
void sha256(const void* data, size_t len, uint8_t out[SHA256_DIGEST_LEN]);

// HMAC-SHA256 (RFC 2104).
void hmac_sha256(const uint8_t* key, size_t key_len, const void* data, size_t data_len,
                 uint8_t out[SHA256_DIGEST_LEN]);

// Lowercase hex of `len` digest bytes into `out`, which needs 2*len+1 bytes.
// Writes "" if it does not fit, never a half-length string that would compare
// equal to nothing and unequal to everything.
void sha256_to_hex(const uint8_t* digest, size_t len, char* out, size_t cap);
