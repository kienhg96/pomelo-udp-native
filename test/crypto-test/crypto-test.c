#include <string.h>
#include "pomelo-test.h"
#include "pomelo/random.h"
#include "pomelo/address.h"
#include "pomelo/constants.h"
#include "crypto/crypto.h"
#include "crypto/token.h"
#include "crypto/checksum.h"
#include "pomelo/base64.h"
#include "base/payload.h"
#include "base/constants.h"


static uint64_t sequence;
static uint8_t nonce[12];
static uint8_t key[32];
static uint8_t ad[16];


static char raw_msg[] = "Hello Crypto!";
static char decrypted_msg[sizeof(raw_msg)];
static char encrypted_msg[sizeof(raw_msg) + POMELO_CRYPTO_AEAD_HMAC_BYTES];


static int test_decode_addresses_rejects_overflow(void) {
    pomelo_track_function();

    // Craft a payload whose address count exceeds the maximum.
    uint8_t buf[64];
    pomelo_payload_t payload;
    payload.data = buf;
    payload.capacity = sizeof(buf);
    payload.position = 0;
    pomelo_payload_write_uint32(
        &payload,
        (uint32_t) (POMELO_CONNECT_TOKEN_MAX_ADDRESSES + 1)
    );

    payload.position = 0; // rewind for reading
    int naddresses = -1;
    pomelo_address_t addresses[POMELO_CONNECT_TOKEN_MAX_ADDRESSES];
    int ret = pomelo_codec_decode_server_addresses(
        &payload, &naddresses, addresses
    );
    pomelo_check(ret != 0); // must be rejected, not overflow
    return 0;
}

static int test_decode_addresses_roundtrip(void) {
    pomelo_track_function();

    pomelo_connect_token_t token;
    memset(&token, 0, sizeof(token));
    token.naddresses = 2;
    pomelo_check(
        pomelo_address_from_string(&token.addresses[0], "127.0.0.1:8000") == 0
    );
    pomelo_check(
        pomelo_address_from_string(&token.addresses[1], "127.0.0.1:8001") == 0
    );

    uint8_t buf[256];
    pomelo_payload_t payload;
    payload.data = buf;
    payload.capacity = sizeof(buf);
    payload.position = 0;
    pomelo_codec_encode_server_addresses(&payload, &token);

    payload.position = 0; // rewind for reading
    int naddresses = 0;
    pomelo_address_t out[POMELO_CONNECT_TOKEN_MAX_ADDRESSES];
    int ret = pomelo_codec_decode_server_addresses(&payload, &naddresses, out);
    pomelo_check(ret == 0);
    pomelo_check(naddresses == 2);
    return 0;
}

static int test_decode_public_rejects_bad_version(void) {
    pomelo_track_function();

    uint8_t buf[POMELO_CONNECT_TOKEN_BYTES];
    memset(buf, 0, sizeof(buf));
    // Corrupt the version-info prefix so it never matches.
    memset(buf, 0xAB, POMELO_VERSION_INFO_BYTES);

    pomelo_connect_token_t token;
    memset(&token, 0, sizeof(token));

    int ret = pomelo_connect_token_decode_public(buf, &token);
    pomelo_check(ret < 0); // bad version must be rejected
    return 0;
}

static int test_random_deterministic_uses_full_seed(void) {
    pomelo_track_function();

    // Two distinct 64-bit seeds that collapse to the SAME value under the
    // old broken (high32 & low32) mixing, but differ as full 64-bit seeds.
    uint8_t a[64];
    uint8_t b[64];
    pomelo_random_buffer_deterministic(a, sizeof(a), 0xFFFFFFFF00000001ULL);
    pomelo_random_buffer_deterministic(b, sizeof(b), 0x00000001FFFFFFFFULL);
    pomelo_check(memcmp(a, b, sizeof(a)) != 0); // full seed must matter
    return 0;
}

static int test_random_deterministic_is_repeatable(void) {
    pomelo_track_function();

    uint8_t a[64];
    uint8_t b[64];
    pomelo_random_buffer_deterministic(a, sizeof(a), 0x123456789ABCDEF0ULL);
    pomelo_random_buffer_deterministic(b, sizeof(b), 0x123456789ABCDEF0ULL);
    pomelo_check(memcmp(a, b, sizeof(a)) == 0); // same seed -> same output
    return 0;
}

static int test_token_private_roundtrip(void) {
    pomelo_track_function();

    uint8_t key[POMELO_KEY_BYTES];
    pomelo_random_buffer(key, sizeof(key));

    pomelo_connect_token_t in;
    memset(&in, 0, sizeof(in));
    in.protocol_id = 0x1122334455667788ULL;
    in.expire_timestamp = 9999999999ULL;
    in.client_id = 0x00000000CAFEBABEULL;
    in.timeout = 10;
    in.naddresses = 1;
    pomelo_check(
        pomelo_address_from_string(&in.addresses[0], "127.0.0.1:9000") == 0
    );
    pomelo_random_buffer(in.connect_token_nonce, POMELO_CONNECT_TOKEN_NONCE_BYTES);
    pomelo_random_buffer(in.client_to_server_key, POMELO_KEY_BYTES);
    pomelo_random_buffer(in.server_to_client_key, POMELO_KEY_BYTES);
    pomelo_random_buffer(in.user_data, POMELO_USER_DATA_BYTES);

    uint8_t priv[POMELO_CONNECT_TOKEN_PRIVATE_BYTES];
    int ret = pomelo_codec_encode_private_connect_token(priv, &in, key);
    pomelo_check(ret == 0);

    pomelo_connect_token_t out;
    memset(&out, 0, sizeof(out));
    // AAD inputs must match what was used at encryption time.
    out.protocol_id = in.protocol_id;
    out.expire_timestamp = in.expire_timestamp;
    memcpy(
        out.connect_token_nonce,
        in.connect_token_nonce,
        POMELO_CONNECT_TOKEN_NONCE_BYTES
    );

    ret = pomelo_connect_token_decode_private(priv, &out, key);
    pomelo_check(ret == 0);
    pomelo_check(out.client_id == in.client_id);
    pomelo_check(out.naddresses == 1);
    pomelo_check(
        memcmp(out.client_to_server_key, in.client_to_server_key, POMELO_KEY_BYTES) == 0
    );
    return 0;
}


static int test_aead_rejects_tampered(void) {
    pomelo_track_function();

    uint8_t k[32], n[12], a[16];
    pomelo_random_buffer(k, sizeof(k));
    pomelo_random_buffer(a, sizeof(a));
    pomelo_crypto_make_nonce(n, sizeof(n), 42);

    uint8_t ct[sizeof(raw_msg) + POMELO_CRYPTO_AEAD_HMAC_BYTES];
    size_t ct_len = 0;
    pomelo_check(pomelo_crypto_encrypt_aead(ct, &ct_len,
        (uint8_t *) raw_msg, sizeof(raw_msg), k, n, a, sizeof(a)) == 0);

    uint8_t out[sizeof(raw_msg)];
    size_t out_len = 0;

    // 1) tampered ciphertext
    uint8_t bad[sizeof(ct)];
    memcpy(bad, ct, ct_len);
    bad[0] ^= 0x01;
    pomelo_check(pomelo_crypto_decrypt_aead(out, &out_len, bad, ct_len,
        k, n, a, sizeof(a)) < 0);

    // 2) tampered AAD
    uint8_t bad_ad[16];
    memcpy(bad_ad, a, sizeof(a));
    bad_ad[0] ^= 0x01;
    pomelo_check(pomelo_crypto_decrypt_aead(out, &out_len, ct, ct_len,
        k, n, bad_ad, sizeof(bad_ad)) < 0);

    // 3) wrong key
    uint8_t wrong_k[32];
    pomelo_random_buffer(wrong_k, sizeof(wrong_k));
    pomelo_check(pomelo_crypto_decrypt_aead(out, &out_len, ct, ct_len,
        wrong_k, n, a, sizeof(a)) < 0);

    // Sanity: the un-tampered inputs must still decrypt (fail only on tamper).
    pomelo_check(pomelo_crypto_decrypt_aead(out, &out_len, ct, ct_len,
        k, n, a, sizeof(a)) == 0);
    pomelo_check(out_len == sizeof(raw_msg));
    pomelo_check(memcmp(out, raw_msg, sizeof(raw_msg)) == 0);
    return 0;
}


static int test_token_private_rejects_tamper(void) {
    pomelo_track_function();

    uint8_t skey[POMELO_KEY_BYTES];
    pomelo_random_buffer(skey, sizeof(skey));

    pomelo_connect_token_t in;
    memset(&in, 0, sizeof(in));
    in.protocol_id = 0x1122334455667788ULL;
    in.expire_timestamp = 9999999999ULL;
    in.client_id = 0x00000000CAFEBABEULL;
    in.timeout = 10;
    in.naddresses = 1;
    pomelo_check(
        pomelo_address_from_string(&in.addresses[0], "127.0.0.1:9000") == 0
    );
    pomelo_random_buffer(in.connect_token_nonce, POMELO_CONNECT_TOKEN_NONCE_BYTES);
    pomelo_random_buffer(in.client_to_server_key, POMELO_KEY_BYTES);
    pomelo_random_buffer(in.server_to_client_key, POMELO_KEY_BYTES);
    pomelo_random_buffer(in.user_data, POMELO_USER_DATA_BYTES);

    uint8_t priv[POMELO_CONNECT_TOKEN_PRIVATE_BYTES];
    pomelo_check(pomelo_codec_encode_private_connect_token(priv, &in, skey) == 0);

    // AAD inputs that the decode must match.
    pomelo_connect_token_t out;

    // 1) Decode with a DIFFERENT (wrong) server key must be rejected.
    uint8_t wrong_key[POMELO_KEY_BYTES];
    pomelo_random_buffer(wrong_key, sizeof(wrong_key));
    memset(&out, 0, sizeof(out));
    out.protocol_id = in.protocol_id;
    out.expire_timestamp = in.expire_timestamp;
    memcpy(out.connect_token_nonce, in.connect_token_nonce,
        POMELO_CONNECT_TOKEN_NONCE_BYTES);
    pomelo_check(
        pomelo_connect_token_decode_private(priv, &out, wrong_key) < 0
    );

    // 2) Decode a byte-flipped ciphertext with the correct key must be rejected.
    uint8_t tampered[POMELO_CONNECT_TOKEN_PRIVATE_BYTES];
    memcpy(tampered, priv, sizeof(priv));
    tampered[0] ^= 0x01;
    memset(&out, 0, sizeof(out));
    out.protocol_id = in.protocol_id;
    out.expire_timestamp = in.expire_timestamp;
    memcpy(out.connect_token_nonce, in.connect_token_nonce,
        POMELO_CONNECT_TOKEN_NONCE_BYTES);
    pomelo_check(
        pomelo_connect_token_decode_private(tampered, &out, skey) < 0
    );

    // Sanity: the un-tampered token with the correct key still decodes.
    memset(&out, 0, sizeof(out));
    out.protocol_id = in.protocol_id;
    out.expire_timestamp = in.expire_timestamp;
    memcpy(out.connect_token_nonce, in.connect_token_nonce,
        POMELO_CONNECT_TOKEN_NONCE_BYTES);
    pomelo_check(pomelo_connect_token_decode_private(priv, &out, skey) == 0);
    pomelo_check(out.client_id == in.client_id);
    return 0;
}


static int test_challenge_token_roundtrip(void) {
    pomelo_track_function();

    uint8_t ckey[POMELO_KEY_BYTES];
    pomelo_random_buffer(ckey, sizeof(ckey));

    uint64_t token_sequence = 0x0102030405060708ULL;

    pomelo_challenge_token_t in;
    memset(&in, 0, sizeof(in));
    pomelo_random_buffer((uint8_t *) &in.client_id, sizeof(in.client_id));
    pomelo_random_buffer(in.user_data, POMELO_USER_DATA_BYTES);

    // Payload backing buffer must be >= POMELO_CHALLENGE_TOKEN_BYTES.
    uint8_t buf[POMELO_CHALLENGE_TOKEN_BYTES];
    pomelo_payload_t payload;
    payload.data = buf;
    payload.capacity = sizeof(buf);
    payload.position = 0;

    pomelo_check(
        pomelo_codec_encrypt_challenge_token(
            &payload, &in, ckey, token_sequence
        ) == 0
    );

    // Rewind to the start of the ciphertext and decrypt with same key+seq.
    payload.position = 0;
    pomelo_challenge_token_t out;
    memset(&out, 0, sizeof(out));
    pomelo_check(
        pomelo_codec_decrypt_challenge_token(
            &payload, &out, ckey, token_sequence
        ) == 0
    );

    pomelo_check(out.client_id == in.client_id);
    pomelo_check(
        memcmp(out.user_data, in.user_data, POMELO_USER_DATA_BYTES) == 0
    );
    return 0;
}


static int test_challenge_token_rejects_wrong_key(void) {
    pomelo_track_function();

    uint8_t ckey[POMELO_KEY_BYTES];
    pomelo_random_buffer(ckey, sizeof(ckey));

    uint64_t token_sequence = 0xAABBCCDDEEFF0011ULL;

    pomelo_challenge_token_t in;
    memset(&in, 0, sizeof(in));
    pomelo_random_buffer((uint8_t *) &in.client_id, sizeof(in.client_id));
    pomelo_random_buffer(in.user_data, POMELO_USER_DATA_BYTES);

    uint8_t buf[POMELO_CHALLENGE_TOKEN_BYTES];
    pomelo_payload_t payload;
    payload.data = buf;
    payload.capacity = sizeof(buf);
    payload.position = 0;

    pomelo_check(
        pomelo_codec_encrypt_challenge_token(
            &payload, &in, ckey, token_sequence
        ) == 0
    );

    // Decrypt with a DIFFERENT key must be rejected.
    uint8_t wrong_key[POMELO_KEY_BYTES];
    pomelo_random_buffer(wrong_key, sizeof(wrong_key));

    payload.position = 0;
    pomelo_challenge_token_t out;
    memset(&out, 0, sizeof(out));
    pomelo_check(
        pomelo_codec_decrypt_challenge_token(
            &payload, &out, wrong_key, token_sequence
        ) < 0
    );
    return 0;
}


static int test_challenge_token_rejects_tamper(void) {
    pomelo_track_function();

    uint8_t ckey[POMELO_KEY_BYTES];
    pomelo_random_buffer(ckey, sizeof(ckey));

    uint64_t token_sequence = 0x1020304050607080ULL;

    pomelo_challenge_token_t in;
    memset(&in, 0, sizeof(in));
    pomelo_random_buffer((uint8_t *) &in.client_id, sizeof(in.client_id));
    pomelo_random_buffer(in.user_data, POMELO_USER_DATA_BYTES);

    uint8_t buf[POMELO_CHALLENGE_TOKEN_BYTES];
    pomelo_payload_t payload;
    payload.data = buf;
    payload.capacity = sizeof(buf);
    payload.position = 0;

    pomelo_check(
        pomelo_codec_encrypt_challenge_token(
            &payload, &in, ckey, token_sequence
        ) == 0
    );

    // Flip one ciphertext byte in place, then decrypt with correct key+seq.
    buf[0] ^= 0x01;

    payload.position = 0;
    pomelo_challenge_token_t out;
    memset(&out, 0, sizeof(out));
    pomelo_check(
        pomelo_codec_decrypt_challenge_token(
            &payload, &out, ckey, token_sequence
        ) < 0
    );
    return 0;
}


static int test_challenge_token_rejects_wrong_sequence(void) {
    pomelo_track_function();

    uint8_t ckey[POMELO_KEY_BYTES];
    pomelo_random_buffer(ckey, sizeof(ckey));

    uint64_t token_sequence = 0x00000000DEADBEEFULL;

    pomelo_challenge_token_t in;
    memset(&in, 0, sizeof(in));
    pomelo_random_buffer((uint8_t *) &in.client_id, sizeof(in.client_id));
    pomelo_random_buffer(in.user_data, POMELO_USER_DATA_BYTES);

    uint8_t buf[POMELO_CHALLENGE_TOKEN_BYTES];
    pomelo_payload_t payload;
    payload.data = buf;
    payload.capacity = sizeof(buf);
    payload.position = 0;

    pomelo_check(
        pomelo_codec_encrypt_challenge_token(
            &payload, &in, ckey, token_sequence
        ) == 0
    );

    // Decrypt with correct key but wrong sequence (wrong nonce) must reject.
    payload.position = 0;
    pomelo_challenge_token_t out;
    memset(&out, 0, sizeof(out));
    pomelo_check(
        pomelo_codec_decrypt_challenge_token(
            &payload, &out, ckey, token_sequence + 1
        ) < 0
    );
    return 0;
}


static int test_checksum_blake2b(void) {
    pomelo_track_function();

    const uint8_t msg_a[] = "The quick brown fox jumps over the lazy dog";
    const uint8_t msg_b[] = "The quick brown fox jumps over the lazy cog";
    const size_t len_a = sizeof(msg_a) - 1; // drop NUL terminator
    const size_t len_b = sizeof(msg_b) - 1;

    // Helper: compute a digest in a single update call.
    uint8_t digest1[POMELO_CRYPTO_CHECKSUM_BYTES];
    pomelo_crypto_checksum_state_t s;
    pomelo_check(pomelo_crypto_checksum_init(&s) == 0);
    pomelo_check(pomelo_crypto_checksum_update(&s, msg_a, len_a) == 0);
    pomelo_check(pomelo_crypto_checksum_final(&s, digest1) == 0);

    // Same input must yield the same digest.
    uint8_t digest2[POMELO_CRYPTO_CHECKSUM_BYTES];
    pomelo_check(pomelo_crypto_checksum_init(&s) == 0);
    pomelo_check(pomelo_crypto_checksum_update(&s, msg_a, len_a) == 0);
    pomelo_check(pomelo_crypto_checksum_final(&s, digest2) == 0);
    pomelo_check(
        memcmp(digest1, digest2, POMELO_CRYPTO_CHECKSUM_BYTES) == 0
    );

    // Streaming the same input in two chunks must match the one-shot digest.
    uint8_t digest_chunked[POMELO_CRYPTO_CHECKSUM_BYTES];
    size_t split = len_a / 2;
    pomelo_check(pomelo_crypto_checksum_init(&s) == 0);
    pomelo_check(pomelo_crypto_checksum_update(&s, msg_a, split) == 0);
    pomelo_check(
        pomelo_crypto_checksum_update(&s, msg_a + split, len_a - split) == 0
    );
    pomelo_check(pomelo_crypto_checksum_final(&s, digest_chunked) == 0);
    pomelo_check(
        memcmp(digest1, digest_chunked, POMELO_CRYPTO_CHECKSUM_BYTES) == 0
    );

    // A one-byte difference in the input must change the digest.
    uint8_t digest3[POMELO_CRYPTO_CHECKSUM_BYTES];
    pomelo_check(pomelo_crypto_checksum_init(&s) == 0);
    pomelo_check(pomelo_crypto_checksum_update(&s, msg_b, len_b) == 0);
    pomelo_check(pomelo_crypto_checksum_final(&s, digest3) == 0);
    pomelo_check(
        memcmp(digest1, digest3, POMELO_CRYPTO_CHECKSUM_BYTES) != 0
    );

    return 0;
}


static int test_base64_roundtrip(void) {
    pomelo_track_function();

    // Cover lengths whose remainder mod 3 is 0, 1 and 2 (padding variants).
    static const size_t lengths[] = { 1, 2, 3, 4, 5, 6, 7, 16, 32 };
    const size_t nlengths = sizeof(lengths) / sizeof(lengths[0]);

    for (size_t li = 0; li < nlengths; ++li) {
        size_t len = lengths[li];

        uint8_t bin[32];
        for (size_t i = 0; i < len; ++i) {
            bin[i] = (uint8_t) (i * 7 + 3);
        }

        char b64[128];
        pomelo_check(
            pomelo_base64_encode(b64, sizeof(b64), bin, len) == 0
        );

        // Encoded length (with padding) must match the documented formula,
        // minus the trailing NUL that the macro accounts for.
        size_t expected = pomelo_base64_calc_encoded_length(len);
        pomelo_check(strlen(b64) == expected - 1);

        // Padded roundtrip: decode back and compare to the original.
        uint8_t out[32];
        pomelo_check(
            pomelo_base64_decode(out, sizeof(out), b64, strlen(b64)) == 0
        );
        pomelo_check(memcmp(out, bin, len) == 0);

        // No-padding edge: strip any '=' and decode the shorter string, which
        // drives the VARIANT_URLSAFE_NO_PADDING branch (b64_len % 4 != 0).
        char stripped[128];
        memcpy(stripped, b64, strlen(b64) + 1);
        size_t slen = strlen(stripped);
        while (slen > 0 && stripped[slen - 1] == '=') {
            stripped[--slen] = '\0';
        }

        uint8_t out2[32];
        pomelo_check(
            pomelo_base64_decode(out2, sizeof(out2), stripped, slen) == 0
        );
        pomelo_check(memcmp(out2, bin, len) == 0);
    }

    return 0;
}


int main(void) {
    printf("Crypto test\n");
    pomelo_check(pomelo_crypto_init() == 0);

    pomelo_run_test(test_decode_addresses_rejects_overflow);
    pomelo_run_test(test_decode_addresses_roundtrip);
    pomelo_run_test(test_decode_public_rejects_bad_version);
    pomelo_run_test(test_random_deterministic_uses_full_seed);
    pomelo_run_test(test_random_deterministic_is_repeatable);
    pomelo_run_test(test_token_private_roundtrip);
    pomelo_run_test(test_aead_rejects_tampered);
    pomelo_run_test(test_token_private_rejects_tamper);
    pomelo_run_test(test_challenge_token_roundtrip);
    pomelo_run_test(test_challenge_token_rejects_wrong_key);
    pomelo_run_test(test_challenge_token_rejects_tamper);
    pomelo_run_test(test_challenge_token_rejects_wrong_sequence);
    pomelo_run_test(test_checksum_blake2b);
    pomelo_run_test(test_base64_roundtrip);

    pomelo_random_buffer(key, sizeof(key));
    pomelo_random_buffer(ad, sizeof(ad));
    pomelo_random_buffer(&sequence, sizeof(sequence));

    pomelo_crypto_make_nonce(nonce, sizeof(nonce), sequence);

    printf("Encrypting message\n");
    size_t output_length = 0;
    int ret = pomelo_crypto_encrypt_aead(
        (uint8_t *) encrypted_msg,
        &output_length,
        (uint8_t *) raw_msg,
        sizeof(raw_msg),
        key,
        nonce,
        ad,
        sizeof(ad)
    );
    pomelo_check(ret == 0);
    pomelo_check(
        output_length == sizeof(raw_msg) + POMELO_CRYPTO_AEAD_HMAC_BYTES
    );

    printf("Decrypting message\n");
    ret = pomelo_crypto_decrypt_aead(
        (uint8_t *) decrypted_msg,
        &output_length,
        (uint8_t *) encrypted_msg,
        sizeof(encrypted_msg),
        key,
        nonce,
        ad,
        sizeof(ad)
    );
    pomelo_check(ret == 0);
    pomelo_check(output_length == sizeof(raw_msg));
    pomelo_check(memcmp(raw_msg, decrypted_msg, sizeof(raw_msg)) == 0);

    printf("*** All crypto tests passed ***\n");
    return 0;
}
