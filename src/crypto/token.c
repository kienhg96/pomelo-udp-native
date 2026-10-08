#include <assert.h>
#include <string.h>
#include "sodium/crypto_aead_xchacha20poly1305.h"
#include "sodium/crypto_aead_chacha20poly1305.h"
#include "base/constants.h"
#include "base/payload.h"
#include "token.h"
#include "crypto.h"


/// @brief The length of associated data for private part of token:
/// version info + protocol id + expire timestamp
#define TOKEN_PRIVATE_ASSOCIATED_DATA_BYTES (POMELO_VERSION_INFO_BYTES + 8 + 8)


int pomelo_connect_token_encode(
    uint8_t * buffer,
    pomelo_connect_token_t * token,
    const uint8_t * key
) {
    assert(buffer != NULL);
    assert(token != NULL);
    assert(key != NULL);

    // Wrap the data to a payload for writing
    pomelo_payload_t payload;
    payload.capacity = POMELO_CONNECT_TOKEN_BYTES;
    payload.data = buffer;
    payload.position = 0;

    // version info (13 bytes)
    pomelo_payload_write_buffer(
        &payload,
        (const uint8_t *) POMELO_VERSION_INFO,
        POMELO_VERSION_INFO_BYTES
    );

    // protocol id
    pomelo_payload_write_uint64(&payload, token->protocol_id);

    // create timestamp
    pomelo_payload_write_uint64(&payload, token->create_timestamp);

    // expire timestamp
    pomelo_payload_write_uint64(&payload, token->expire_timestamp);

    // connect token nonce
    pomelo_payload_write_buffer(
        &payload,
        token->connect_token_nonce,
        POMELO_CONNECT_TOKEN_NONCE_BYTES
    );

    // encrypted private connect token data
    int ret = pomelo_codec_encode_private_connect_token(
        payload.data + payload.position,
        token,
        key
    );

    if (ret < 0) {
        return ret;
    }

    payload.position += POMELO_CONNECT_TOKEN_PRIVATE_BYTES;

    // timeout seconds
    pomelo_payload_write_int32(&payload, token->timeout);

    // server addresses
    pomelo_codec_encode_server_addresses(&payload, token);

    // client to server key
    pomelo_payload_write_buffer(
        &payload,
        token->client_to_server_key,
        POMELO_KEY_BYTES
    );

    // server to client key
    pomelo_payload_write_buffer(
        &payload,
        token->server_to_client_key,
        POMELO_KEY_BYTES
    );

    // zero pad to 2048 bytes
    pomelo_payload_zero_pad(&payload, POMELO_CONNECT_TOKEN_BYTES);

    return 0;
}


int pomelo_codec_encode_private_connect_token(
    uint8_t * buffer,
    pomelo_connect_token_t * token,
    const uint8_t * key
) {
    assert(buffer != NULL);
    assert(token != NULL);
    assert(key != NULL);

    // Setup payload for writing
    pomelo_payload_t payload;
    payload.data = buffer;
    payload.capacity = POMELO_CONNECT_TOKEN_PRIVATE_BYTES;
    payload.position = 0;

    // client id (int64)
    pomelo_payload_write_int64(&payload, token->client_id);

    // timeout seconds (int32)
    pomelo_payload_write_int32(&payload, token->timeout);

    // server addresses
    pomelo_codec_encode_server_addresses(&payload, token);

    // client to server key
    pomelo_payload_write_buffer(
        &payload,
        token->client_to_server_key,
        POMELO_KEY_BYTES
    );

    // server to client key
    pomelo_payload_write_buffer(
        &payload,
        token->server_to_client_key,
        POMELO_KEY_BYTES
    );

    // user data
    pomelo_payload_write_buffer(
        &payload,
        token->user_data,
        POMELO_USER_DATA_BYTES
    );

    // zero pad to 1024 bytes
    pomelo_payload_zero_pad(&payload, POMELO_CONNECT_TOKEN_PRIVATE_BYTES);

    // The private data is encrypted using
    // crypto_aead_xchacha20poly1305_ietf_encrypt
    // with following associated data:

    // Build the associated data
    uint8_t associated_data[TOKEN_PRIVATE_ASSOCIATED_DATA_BYTES];
    pomelo_codec_encode_connect_token_associated_data(associated_data, token);

    unsigned long long encrypted_length;
    int ret = crypto_aead_xchacha20poly1305_ietf_encrypt(
        buffer,
        &encrypted_length,
        buffer,
        POMELO_CONNECT_TOKEN_PRIVATE_BYTES -
            crypto_aead_xchacha20poly1305_ietf_ABYTES,
        associated_data,
        TOKEN_PRIVATE_ASSOCIATED_DATA_BYTES,
        NULL,
        token->connect_token_nonce,
        key
    );

    return ret;
}


int pomelo_connect_token_decode_private(
    const uint8_t * buffer,
    pomelo_connect_token_t * token,
    const uint8_t * key
) {
    assert(buffer != NULL);
    assert(token != NULL);
    assert(key != NULL);

    unsigned long long decrypted_length;
    uint8_t associated_data[TOKEN_PRIVATE_ASSOCIATED_DATA_BYTES];

    pomelo_codec_encode_connect_token_associated_data(
        associated_data,
        token
    );

    // Store decrypted data of the private portion
    uint8_t decrypted[POMELO_CONNECT_TOKEN_PRIVATE_BYTES];

    // Decrypt the private connect token
    int ret = crypto_aead_xchacha20poly1305_ietf_decrypt(
        decrypted,
        &decrypted_length,
        NULL,
        buffer,
        POMELO_CONNECT_TOKEN_PRIVATE_BYTES,
        associated_data,
        TOKEN_PRIVATE_ASSOCIATED_DATA_BYTES,
        token->connect_token_nonce,
        key
    );

    if (ret != 0) {
        return ret;
    }

    pomelo_payload_t payload;
    payload.capacity = POMELO_CONNECT_TOKEN_PRIVATE_BYTES;
    payload.position = 0;
    payload.data = decrypted;

    // client id (Only visible in private part)
    pomelo_payload_read_int64(&payload, &token->client_id);

    // timeout seconds
    pomelo_payload_read_int32(&payload, &token->timeout);

    // server addresses
    if (pomelo_codec_decode_server_addresses(
            &payload,
            &token->naddresses,
            token->addresses
        ) != 0) {
        return -1;
    }

    // client to server key
    pomelo_payload_read_buffer(
        &payload,
        token->client_to_server_key,
        POMELO_KEY_BYTES
    );

    // server to client key
    pomelo_payload_read_buffer(
        &payload,
        token->server_to_client_key,
        POMELO_KEY_BYTES
    );

    // user data
    pomelo_payload_read_buffer(
        &payload,
        token->user_data,
        POMELO_USER_DATA_BYTES
    );

    return 0;
}


void pomelo_codec_encode_server_addresses(
    pomelo_payload_t * payload,
    pomelo_connect_token_t * info
) {
    assert(payload != NULL);
    assert(info != NULL);

    // number of server addresses
    pomelo_payload_write_uint32(payload, info->naddresses);

    // <for each server address>
    pomelo_address_t * address;
    for (int i = 0; i < info->naddresses; ++i) {
        address = info->addresses + i;

        // address type
        pomelo_payload_write_uint8(payload, (uint8_t) address->type);

        pomelo_address_ip_t ip = { 0 };
        pomelo_address_ip(address, &ip); // extract IP from address

        // ip address
        if (address->type == POMELO_ADDRESS_IPV4) {
            // for IPv4
            pomelo_payload_write_uint8(payload, ip.v4[0]);
            pomelo_payload_write_uint8(payload, ip.v4[1]);
            pomelo_payload_write_uint8(payload, ip.v4[2]);
            pomelo_payload_write_uint8(payload, ip.v4[3]);
        } else {
            // for IPv6
            pomelo_payload_write_uint16(payload, ip.v6[0]);
            pomelo_payload_write_uint16(payload, ip.v6[1]);
            pomelo_payload_write_uint16(payload, ip.v6[2]);
            pomelo_payload_write_uint16(payload, ip.v6[3]);
            pomelo_payload_write_uint16(payload, ip.v6[4]);
            pomelo_payload_write_uint16(payload, ip.v6[5]);
            pomelo_payload_write_uint16(payload, ip.v6[6]);
            pomelo_payload_write_uint16(payload, ip.v6[7]);
        }

        uint16_t port = pomelo_address_port(address);
        pomelo_payload_write_uint16(payload, port);
    }
}


void pomelo_codec_decode_server_address(
    pomelo_payload_t * payload,
    pomelo_address_t * address
) {
    assert(payload != NULL);
    assert(address != NULL);

    uint8_t value = 0;
    pomelo_payload_read_uint8(payload, &value);

    uint8_t type = value;
    pomelo_address_ip_t ip = { 0 };
    uint16_t port = 0;
    if (type == POMELO_ADDRESS_IPV4) {
        // IPv4
        pomelo_payload_read_uint8(payload, &ip.v4[0]);
        pomelo_payload_read_uint8(payload, &ip.v4[1]);
        pomelo_payload_read_uint8(payload, &ip.v4[2]);
        pomelo_payload_read_uint8(payload, &ip.v4[3]);
    } else {
        // IPv6
        pomelo_payload_read_uint16(payload, &ip.v6[0]);
        pomelo_payload_read_uint16(payload, &ip.v6[1]);
        pomelo_payload_read_uint16(payload, &ip.v6[2]);
        pomelo_payload_read_uint16(payload, &ip.v6[3]);
        pomelo_payload_read_uint16(payload, &ip.v6[4]);
        pomelo_payload_read_uint16(payload, &ip.v6[5]);
        pomelo_payload_read_uint16(payload, &ip.v6[6]);
        pomelo_payload_read_uint16(payload, &ip.v6[7]);
    }

    pomelo_payload_read_uint16(payload, &port);

    // Finally, set the value
    pomelo_address_set(address, type, &ip, port);
}


int pomelo_codec_decode_server_addresses(
    pomelo_payload_t * payload,
    int * naddresses,
    pomelo_address_t * addresses
) {
    assert(payload != NULL);
    assert(naddresses != NULL);
    assert(addresses != NULL);

    *naddresses = 0;

    // number of server addresses
    uint32_t naddr = 0;
    if (pomelo_payload_read_uint32(payload, &naddr) != 0) {
        return -1; // not enough data
    }

    // Reject counts outside the valid range (netcode mandates this).
    if (naddr == 0 || naddr > POMELO_CONNECT_TOKEN_MAX_ADDRESSES) {
        return -1;
    }

    // <for each server address>
    for (uint32_t i = 0; i < naddr; ++i) {
        pomelo_codec_decode_server_address(payload, addresses + i);
    }

    *naddresses = (int) naddr;
    return 0;
}


void pomelo_codec_encode_connect_token_associated_data(
    uint8_t * associated_data,
    pomelo_connect_token_t * info
) {
    assert(associated_data != NULL);
    assert(info != NULL);

    pomelo_payload_t payload;
    payload.capacity = TOKEN_PRIVATE_ASSOCIATED_DATA_BYTES;
    payload.position = 0;
    payload.data = associated_data;

    // version info (13 bytes)
    pomelo_payload_write_buffer(
        &payload,
        (const uint8_t *) POMELO_VERSION_INFO,
        POMELO_VERSION_INFO_BYTES
    );

    // protocol id
    pomelo_payload_write_uint64(&payload, info->protocol_id);

    // expire timestamp
    pomelo_payload_write_uint64(&payload, info->expire_timestamp);
}


int pomelo_connect_token_decode_public(
    const uint8_t * buffer,
    pomelo_connect_token_t * token
) {
    assert(buffer != NULL);
    assert(token != NULL);

    pomelo_payload_t payload;
    payload.position = 0;
    payload.capacity = POMELO_CONNECT_TOKEN_BYTES;
    payload.data = (uint8_t *) buffer; // Just for reading

    // Check the version info
    int cmp = memcmp(buffer, POMELO_VERSION_INFO, POMELO_VERSION_INFO_BYTES);
    if (cmp != 0) { // Not equals -> invalid token
        return -1;
    }

    payload.position += POMELO_VERSION_INFO_BYTES;

    // protocol id
    pomelo_payload_read_uint64(&payload, &token->protocol_id);

    // create timestamp
    pomelo_payload_read_uint64(&payload, &token->create_timestamp);

    // expire timestamp
    pomelo_payload_read_uint64(&payload, &token->expire_timestamp);

    // connect token nonce
    pomelo_payload_read_buffer(
        &payload,
        token->connect_token_nonce,
        POMELO_CONNECT_TOKEN_NONCE_BYTES
    );

    // Skip the private connect token data
    payload.position += POMELO_CONNECT_TOKEN_PRIVATE_BYTES;

    // timeout seconds
    pomelo_payload_read_int32(&payload, &token->timeout);

    // server addresses
    if (pomelo_codec_decode_server_addresses(
            &payload,
            &token->naddresses,
            token->addresses
        ) != 0) {
        return -1;
    }

    // client to server key
    pomelo_payload_read_buffer(
        &payload,
        token->client_to_server_key,
        POMELO_KEY_BYTES
    );

    // server to client key
    pomelo_payload_read_buffer(
        &payload,
        token->server_to_client_key,
        POMELO_KEY_BYTES
    );

    return 0;
}


int pomelo_codec_encrypt_challenge_token(
    pomelo_payload_t * payload,
    pomelo_challenge_token_t * token,
    const uint8_t * key,
    uint64_t token_sequence
) {
    assert(payload != NULL);
    assert(token != NULL);
    assert(key != NULL);

    // Save the current position for encrypting
    size_t begin = payload->position;

    // client id
    pomelo_payload_write_int64(payload, token->client_id);

    // user data
    pomelo_payload_write_buffer(
        payload,
        token->user_data,
        POMELO_USER_DATA_BYTES
    );

    // <zero pad to 300 bytes>
    pomelo_payload_zero_pad(payload, begin + POMELO_CHALLENGE_TOKEN_BYTES);

    // The nonce of encryption
    uint8_t nonce[crypto_aead_chacha20poly1305_IETF_NPUBBYTES];

    // Process nonce
    pomelo_crypto_make_nonce(nonce, sizeof(nonce), token_sequence);

    // The beginning of buffer to be encrypted
    void * buffer = payload->data + begin;

    unsigned long long encrypted_len;
    return crypto_aead_chacha20poly1305_ietf_encrypt(
        buffer,
        &encrypted_len,
        buffer,
        POMELO_CHALLENGE_TOKEN_BYTES -
            crypto_aead_chacha20poly1305_ietf_ABYTES,
        NULL, // No associated data
        0,
        NULL,
        nonce,
        key
    );
}


int pomelo_codec_decrypt_challenge_token(
    pomelo_payload_t * payload,
    pomelo_challenge_token_t * token,
    const uint8_t * key,
    uint64_t token_sequence
) {
    assert(payload != NULL);
    assert(token != NULL);
    assert(key != NULL);

    // Save the begin position of payload
    size_t begin = payload->position;
    uint8_t * buffer = payload->data + begin;

    // The nonce of decryption
    uint8_t nonce[crypto_aead_chacha20poly1305_IETF_NPUBBYTES];

    // Process nonce
    pomelo_crypto_make_nonce(nonce, sizeof(nonce), token_sequence);

    unsigned long long decrypted_len;
    int ret = crypto_aead_chacha20poly1305_ietf_decrypt(
        buffer,
        &decrypted_len,
        NULL,
        buffer,
        POMELO_CHALLENGE_TOKEN_BYTES,
        NULL,
        0,
        nonce,
        key
    );
    if (ret < 0) return -1;

    // client id
    pomelo_payload_read_int64(payload, &token->client_id);

    // user data
    pomelo_payload_read_buffer(
        payload,
        token->user_data,
        POMELO_USER_DATA_BYTES
    );

    // Finally, seek the position to the end of buffer
    payload->position = begin + POMELO_CHALLENGE_TOKEN_BYTES;

    return 0;
}
