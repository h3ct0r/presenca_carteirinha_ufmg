#include "app/sha256.h"

#include <string.h>

// Straight FIPS 180-4 SHA-256. Kept deliberately plain — this is verified
// against published vectors rather than read for cleverness.

namespace {

constexpr uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4,
    0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe,
    0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f,
    0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
    0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
    0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116,
    0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
    0xc67178f2};

inline uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

void transform(uint32_t state[8], const uint8_t block[64]) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++) {
        w[i] = ((uint32_t)block[i * 4] << 24) | ((uint32_t)block[i * 4 + 1] << 16) |
               ((uint32_t)block[i * 4 + 2] << 8) | (uint32_t)block[i * 4 + 3];
    }
    for (int i = 16; i < 64; i++) {
        const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
    uint32_t e = state[4], f = state[5], g = state[6], h = state[7];
    for (int i = 0; i < 64; i++) {
        const uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        const uint32_t ch = (e & f) ^ ((~e) & g);
        const uint32_t t1 = h + S1 + ch + K[i] + w[i];
        const uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        const uint32_t t2 = S0 + maj;
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    state[5] += f;
    state[6] += g;
    state[7] += h;
}

}  // namespace

void sha256_init(sha256_ctx_t* c) {
    c->state[0] = 0x6a09e667;
    c->state[1] = 0xbb67ae85;
    c->state[2] = 0x3c6ef372;
    c->state[3] = 0xa54ff53a;
    c->state[4] = 0x510e527f;
    c->state[5] = 0x9b05688c;
    c->state[6] = 0x1f83d9ab;
    c->state[7] = 0x5be0cd19;
    c->bitlen = 0;
    c->buflen = 0;
}

void sha256_update(sha256_ctx_t* c, const void* data, size_t len) {
    const uint8_t* p = (const uint8_t*)data;
    for (size_t i = 0; i < len; i++) {
        c->buf[c->buflen++] = p[i];
        if (c->buflen == SHA256_BLOCK_LEN) {
            transform(c->state, c->buf);
            c->bitlen += 512;
            c->buflen = 0;
        }
    }
}

void sha256_final(sha256_ctx_t* c, uint8_t out[SHA256_DIGEST_LEN]) {
    size_t i = c->buflen;
    c->bitlen += (uint64_t)c->buflen * 8;

    // 0x80, then zeroes, leaving 8 bytes for the length. When the length would
    // not fit in this block, finish it and pad a second one.
    c->buf[i++] = 0x80;
    if (i > 56) {
        while (i < SHA256_BLOCK_LEN) c->buf[i++] = 0;
        transform(c->state, c->buf);
        i = 0;
    }
    while (i < 56) c->buf[i++] = 0;
    for (int b = 7; b >= 0; b--) c->buf[i++] = (uint8_t)(c->bitlen >> (b * 8));
    transform(c->state, c->buf);

    for (int j = 0; j < 8; j++) {
        out[j * 4 + 0] = (uint8_t)(c->state[j] >> 24);
        out[j * 4 + 1] = (uint8_t)(c->state[j] >> 16);
        out[j * 4 + 2] = (uint8_t)(c->state[j] >> 8);
        out[j * 4 + 3] = (uint8_t)(c->state[j]);
    }
}

void sha256(const void* data, size_t len, uint8_t out[SHA256_DIGEST_LEN]) {
    sha256_ctx_t c;
    sha256_init(&c);
    sha256_update(&c, data, len);
    sha256_final(&c, out);
}

void hmac_sha256(const uint8_t* key, size_t key_len, const void* data, size_t data_len,
                 uint8_t out[SHA256_DIGEST_LEN]) {
    uint8_t k[SHA256_BLOCK_LEN] = {0};
    // RFC 2104: a key longer than the block is hashed first; a shorter one is
    // zero-padded to the block length.
    if (key_len > SHA256_BLOCK_LEN) {
        sha256(key, key_len, k);
    } else if (key && key_len) {
        memcpy(k, key, key_len);
    }

    uint8_t ipad[SHA256_BLOCK_LEN], opad[SHA256_BLOCK_LEN];
    for (size_t i = 0; i < SHA256_BLOCK_LEN; i++) {
        ipad[i] = (uint8_t)(k[i] ^ 0x36);
        opad[i] = (uint8_t)(k[i] ^ 0x5c);
    }

    uint8_t inner[SHA256_DIGEST_LEN];
    sha256_ctx_t c;
    sha256_init(&c);
    sha256_update(&c, ipad, sizeof(ipad));
    sha256_update(&c, data, data_len);
    sha256_final(&c, inner);

    sha256_init(&c);
    sha256_update(&c, opad, sizeof(opad));
    sha256_update(&c, inner, sizeof(inner));
    sha256_final(&c, out);

    // The derived key material is the secret here; don't leave it on the stack.
    memset(k, 0, sizeof(k));
    memset(ipad, 0, sizeof(ipad));
    memset(opad, 0, sizeof(opad));
}

void sha256_to_hex(const uint8_t* digest, size_t len, char* out, size_t cap) {
    if (!out || !cap) return;
    out[0] = '\0';
    if (!digest || cap < len * 2 + 1) return;  // all or nothing
    static const char HEX[] = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        out[i * 2] = HEX[digest[i] >> 4];
        out[i * 2 + 1] = HEX[digest[i] & 0x0F];
    }
    out[len * 2] = '\0';
}
