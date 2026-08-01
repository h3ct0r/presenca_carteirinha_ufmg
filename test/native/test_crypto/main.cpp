// SHA-256 / HMAC-SHA256 against published vectors, and the credential
// fingerprints built on them.
//
// The known-answer tests matter more than usual: every card binding and every
// password on the device depends on this being right, a wrong implementation
// still produces confident-looking hex, and nothing else in the system would
// notice. Vectors are from FIPS 180-2 (SHA-256) and RFC 4231 (HMAC-SHA256).

#include <string.h>
#include <unity.h>

#include "app/credential.h"
#include "app/roster.h"
#include "app/sha256.h"
#include "services/device_secret.h"

void setUp(void) {}
void tearDown(void) {}

static void hex_of(const uint8_t* d, size_t n, char* out, size_t cap) {
    sha256_to_hex(d, n, out, cap);
}

// --- SHA-256 known answers --------------------------------------------------

static void assert_sha256(const char* msg, const char* expect) {
    uint8_t d[SHA256_DIGEST_LEN];
    sha256(msg, strlen(msg), d);
    char hex[SHA256_DIGEST_LEN * 2 + 1];
    hex_of(d, sizeof(d), hex, sizeof(hex));
    TEST_ASSERT_EQUAL_STRING(expect, hex);
}

static void test_sha256_empty(void) {
    assert_sha256("", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}

static void test_sha256_abc(void) {
    assert_sha256("abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

// Two blocks, exercising the multi-block path.
static void test_sha256_two_blocks(void) {
    assert_sha256("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
                  "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}

// 55 and 56 bytes straddle the padding boundary (the length must spill into a
// second block at 56) — the classic place a hand-written SHA-256 breaks.
static void test_sha256_padding_boundary(void) {
    char m55[56], m56[57];
    memset(m55, 'a', 55);
    m55[55] = '\0';
    memset(m56, 'a', 56);
    m56[56] = '\0';
    assert_sha256(m55, "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318");
    assert_sha256(m56, "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a");
}

// One million 'a' is the FIPS long-message vector; it also proves the streaming
// update path agrees with the one-shot one.
static void test_sha256_streaming_matches_oneshot(void) {
    sha256_ctx_t c;
    sha256_init(&c);
    char chunk[1000];
    memset(chunk, 'a', sizeof(chunk));
    for (int i = 0; i < 1000; i++) sha256_update(&c, chunk, sizeof(chunk));
    uint8_t d[SHA256_DIGEST_LEN];
    sha256_final(&c, d);
    char hex[SHA256_DIGEST_LEN * 2 + 1];
    hex_of(d, sizeof(d), hex, sizeof(hex));
    TEST_ASSERT_EQUAL_STRING("cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0",
                             hex);
}

// --- HMAC-SHA256 known answers (RFC 4231) -----------------------------------

static void test_hmac_rfc4231_case1(void) {
    uint8_t key[20];
    memset(key, 0x0b, sizeof(key));
    uint8_t mac[SHA256_DIGEST_LEN];
    hmac_sha256(key, sizeof(key), "Hi There", 8, mac);
    char hex[SHA256_DIGEST_LEN * 2 + 1];
    hex_of(mac, sizeof(mac), hex, sizeof(hex));
    TEST_ASSERT_EQUAL_STRING("b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7",
                             hex);
}

static void test_hmac_rfc4231_case2(void) {
    uint8_t mac[SHA256_DIGEST_LEN];
    hmac_sha256((const uint8_t*)"Jefe", 4, "what do ya want for nothing?", 28, mac);
    char hex[SHA256_DIGEST_LEN * 2 + 1];
    hex_of(mac, sizeof(mac), hex, sizeof(hex));
    TEST_ASSERT_EQUAL_STRING("5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843",
                             hex);
}

// A key longer than the 64-byte block must be hashed down first (RFC 2104).
static void test_hmac_rfc4231_long_key(void) {
    uint8_t key[131];
    memset(key, 0xaa, sizeof(key));
    uint8_t mac[SHA256_DIGEST_LEN];
    hmac_sha256(key, sizeof(key), "Test Using Larger Than Block-Size Key - Hash Key First", 54,
                mac);
    char hex[SHA256_DIGEST_LEN * 2 + 1];
    hex_of(mac, sizeof(mac), hex, sizeof(hex));
    TEST_ASSERT_EQUAL_STRING("60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54",
                             hex);
}

// --- hex helper -------------------------------------------------------------

static void test_hex_refuses_to_truncate(void) {
    uint8_t d[SHA256_DIGEST_LEN] = {0};
    char small[10];
    sha256_to_hex(d, sizeof(d), small, sizeof(small));
    // "" rather than a short string that would silently compare unequal forever.
    TEST_ASSERT_EQUAL_STRING("", small);
}

// --- uid_fingerprint --------------------------------------------------------

static const uint8_t* test_key(size_t* len) {
    static uint8_t k[DEVICE_SECRET_LEN];
    device_secret_get(k);
    *len = sizeof(k);
    return k;
}

static void fp_uid(const char* uid, char* out, size_t cap) {
    size_t klen = 0;
    const uint8_t* k = test_key(&klen);
    uid_fingerprint(k, klen, uid, out, cap);
}

// The same physical card arrives with different separators and case depending on
// where the string came from; all of them must fingerprint identically or a card
// would stop being recognised depending on who formatted it.
static void test_uid_fingerprint_ignores_format(void) {
    char a[UID_FINGERPRINT_CAP], b[UID_FINGERPRINT_CAP], c[UID_FINGERPRINT_CAP];
    fp_uid("e0:d1-33 5f", a, sizeof(a));
    fp_uid("E0D1335F", b, sizeof(b));
    fp_uid("e0-d1-33-5f", c, sizeof(c));
    TEST_ASSERT_EQUAL_STRING(a, b);
    TEST_ASSERT_EQUAL_STRING(a, c);
    TEST_ASSERT_TRUE(credential_is_fingerprint(a, UID_FINGERPRINT_HEX));
}

static void test_uid_fingerprint_separates_distinct_cards(void) {
    char a[UID_FINGERPRINT_CAP], b[UID_FINGERPRINT_CAP];
    fp_uid("04:A3:1B:2C", a, sizeof(a));
    fp_uid("04:A3:1B:2D", b, sizeof(b));
    TEST_ASSERT_TRUE(strcmp(a, b) != 0);
}

// The whole point: without the device key the value is not reproducible.
static void test_uid_fingerprint_depends_on_the_key(void) {
    char with_real[UID_FINGERPRINT_CAP], with_other[UID_FINGERPRINT_CAP];
    fp_uid("04:A3:1B:2C", with_real, sizeof(with_real));
    uint8_t other[DEVICE_SECRET_LEN];
    memset(other, 0x5A, sizeof(other));
    uid_fingerprint(other, sizeof(other), "04:A3:1B:2C", with_other, sizeof(with_other));
    TEST_ASSERT_TRUE(strcmp(with_real, with_other) != 0);
}

static void test_uid_fingerprint_rejects_unusable_input(void) {
    char out[UID_FINGERPRINT_CAP] = "dirty";
    fp_uid("", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("", out);
    // Only separators: nothing usable survives normalization.
    strcpy(out, "dirty");
    fp_uid(":-  ", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("", out);
}

// A buffer too small yields "" rather than a truncated value that would compare
// unequal to the stored fingerprint and read as "wrong card".
static void test_uid_fingerprint_refuses_to_truncate(void) {
    char tiny[8] = "dirty";
    fp_uid("04:A3:1B:2C", tiny, sizeof(tiny));
    TEST_ASSERT_EQUAL_STRING("", tiny);
}

// It has to fit the existing student_t::rfid_uid[24] with no struct growth —
// 600 of those live in the constrained internal pool, and widening the field
// would cost ~26 KB there. Asserted, not assumed: this is the constraint that
// picked UID_FINGERPRINT_HEX, so it should fail loudly if either side moves.
static void test_uid_fingerprint_fits_the_roster_field(void) {
    TEST_ASSERT_EQUAL_size_t(sizeof(student_t::rfid_uid), UID_FINGERPRINT_CAP);
    char out[UID_FINGERPRINT_CAP];
    fp_uid("04:A3:1B:2C", out, sizeof(out));
    TEST_ASSERT_EQUAL_size_t(UID_FINGERPRINT_CAP - 1, strlen(out));
}

// --- password_hash ----------------------------------------------------------

static void hash_pw(const char* pw, char* out, size_t cap) {
    size_t klen = 0;
    const uint8_t* k = test_key(&klen);
    password_hash(k, klen, pw, out, cap);
}

static void test_password_hash_is_stable_and_distinct(void) {
    char a[PASSWORD_HASH_CAP], b[PASSWORD_HASH_CAP], c[PASSWORD_HASH_CAP];
    hash_pw("123456", a, sizeof(a));
    hash_pw("123456", b, sizeof(b));
    hash_pw("123457", c, sizeof(c));
    TEST_ASSERT_EQUAL_STRING(a, b);
    TEST_ASSERT_TRUE(strcmp(a, c) != 0);
    TEST_ASSERT_TRUE(credential_is_fingerprint(a, PASSWORD_HASH_HEX));
}

// Unlike a UID, a password is taken verbatim — case and separators are part of
// it, so normalizing would silently merge distinct passwords.
static void test_password_hash_is_not_canonicalized(void) {
    char a[PASSWORD_HASH_CAP], b[PASSWORD_HASH_CAP];
    hash_pw("12-34", a, sizeof(a));
    hash_pw("1234", b, sizeof(b));
    TEST_ASSERT_TRUE(strcmp(a, b) != 0);
}

static void test_empty_password_hashes_to_nothing(void) {
    char out[PASSWORD_HASH_CAP] = "dirty";
    hash_pw("", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("", out);  // "no password set"
}

// --- credential_is_fingerprint ----------------------------------------------

static void test_is_fingerprint_rejects_plaintext(void) {
    // The values an authoring tool writes must never be mistaken for converted
    // ones, or the conversion pass would skip them and leave them in the clear.
    TEST_ASSERT_FALSE(credential_is_fingerprint("1234", PASSWORD_HASH_HEX));
    TEST_ASSERT_FALSE(credential_is_fingerprint("E0:D1:33:5F", UID_FINGERPRINT_HEX));
    TEST_ASSERT_FALSE(credential_is_fingerprint("", UID_FINGERPRINT_HEX));
    TEST_ASSERT_FALSE(credential_is_fingerprint(nullptr, UID_FINGERPRINT_HEX));
    // Right shape, wrong length.
    TEST_ASSERT_FALSE(credential_is_fingerprint("v1:abc", UID_FINGERPRINT_HEX));
    // Right length, no prefix.
    TEST_ASSERT_FALSE(credential_is_fingerprint("abcdef0123456789abcdef0", UID_FINGERPRINT_HEX));
    // Uppercase hex is not what we emit, so treat it as foreign.
    TEST_ASSERT_FALSE(credential_is_fingerprint("v1:ABCDEF0123456789ABCDEF", UID_FINGERPRINT_HEX));
    // A uid fingerprint is not a password hash and vice versa.
    char uid[UID_FINGERPRINT_CAP];
    fp_uid("04:A3:1B:2C", uid, sizeof(uid));
    TEST_ASSERT_FALSE(credential_is_fingerprint(uid, PASSWORD_HASH_HEX));
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_sha256_empty);
    RUN_TEST(test_sha256_abc);
    RUN_TEST(test_sha256_two_blocks);
    RUN_TEST(test_sha256_padding_boundary);
    RUN_TEST(test_sha256_streaming_matches_oneshot);
    RUN_TEST(test_hmac_rfc4231_case1);
    RUN_TEST(test_hmac_rfc4231_case2);
    RUN_TEST(test_hmac_rfc4231_long_key);
    RUN_TEST(test_hex_refuses_to_truncate);
    RUN_TEST(test_uid_fingerprint_ignores_format);
    RUN_TEST(test_uid_fingerprint_separates_distinct_cards);
    RUN_TEST(test_uid_fingerprint_depends_on_the_key);
    RUN_TEST(test_uid_fingerprint_rejects_unusable_input);
    RUN_TEST(test_uid_fingerprint_refuses_to_truncate);
    RUN_TEST(test_uid_fingerprint_fits_the_roster_field);
    RUN_TEST(test_password_hash_is_stable_and_distinct);
    RUN_TEST(test_password_hash_is_not_canonicalized);
    RUN_TEST(test_empty_password_hashes_to_nothing);
    RUN_TEST(test_is_fingerprint_rejects_plaintext);
    return UNITY_END();
}
