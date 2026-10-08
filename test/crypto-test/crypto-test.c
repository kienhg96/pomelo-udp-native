#include <string.h>
#include "pomelo-test.h"
#include "pomelo/random.h"
#include "pomelo/address.h"
#include "pomelo/constants.h"
#include "crypto/crypto.h"
#include "crypto/token.h"
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


int main(void) {
    printf("Crypto test\n");
    pomelo_check(pomelo_crypto_init() == 0);

    pomelo_run_test(test_decode_addresses_rejects_overflow);
    pomelo_run_test(test_decode_addresses_roundtrip);
    pomelo_run_test(test_decode_public_rejects_bad_version);
    pomelo_run_test(test_random_deterministic_uses_full_seed);
    pomelo_run_test(test_random_deterministic_is_repeatable);
    pomelo_run_test(test_token_private_roundtrip);

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
