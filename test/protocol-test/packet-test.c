#include <string.h>
#include "pomelo-test.h"
#include "crypto/crypto.h"
#include "protocol/socket.h"
#include "protocol/context.h"
#include "pomelo/random.h"

// Environment
static pomelo_allocator_t * allocator;

// Contexts
static pomelo_buffer_context_t * buffer_ctx;
static pomelo_protocol_context_t * protocol_ctx;
static pomelo_protocol_crypto_context_t * crypto_ctx;

// Connect token
static uint8_t connect_token[POMELO_CONNECT_TOKEN_BYTES];
static pomelo_connect_token_t token;
static pomelo_connect_token_t decoded_token;


/// @brief Encode and encrypt packet
static int encode_and_encrypt_packet(
    pomelo_protocol_packet_t * packet,
    pomelo_buffer_view_t * view
) {
    pomelo_protocol_packet_header_t header;
    pomelo_protocol_packet_header_init(&header, packet);

    int ret = pomelo_protocol_packet_header_encode(&header, view);
    if (ret < 0) return ret;

    pomelo_buffer_view_t body_view;
    body_view.buffer = view->buffer;
    body_view.offset = view->offset + view->length; // Skip header
    body_view.length = 0;

    ret = pomelo_protocol_packet_encode(packet, crypto_ctx, &body_view);
    if (ret < 0) return ret;

    ret = pomelo_protocol_crypto_context_encrypt_packet(
        crypto_ctx,
        &body_view,
        &header
    );
    if (ret < 0) return ret;

    // Update the original view
    view->length += body_view.length;

    return 0;
}


/// @brief Decrypt and decode packet
static int decrypt_and_decode_packet(
    pomelo_protocol_packet_t * packet,
    pomelo_buffer_view_t * body_view,
    pomelo_protocol_packet_header_t * header
) {
    packet->sequence = header->sequence;
    int ret = pomelo_protocol_crypto_context_decrypt_packet(
        crypto_ctx,
        body_view,
        header
    );
    if (ret < 0) return ret;

    ret = pomelo_protocol_packet_decode(packet, crypto_ctx, body_view);
    if (ret < 0) return ret;

    return 0;
}


static uint64_t random_u64(void) {
    uint64_t value = 0;
    pomelo_random_buffer((uint8_t *) &value, sizeof(value));
    return value;
}


static int64_t random_i64(void) {
    int64_t value = 0;
    pomelo_random_buffer((uint8_t *) &value, sizeof(value));
    return value;
}


static int32_t random_i32(void) {
    int32_t value = 0;
    pomelo_random_buffer((uint8_t *) &value, sizeof(value));
    return value;
}


static uint16_t random_u16(void) {
    uint16_t value = 0;
    pomelo_random_buffer((uint8_t *) &value, sizeof(value));
    return value;
}


static uint8_t random_u8(void) {
    uint8_t value = 0;
    pomelo_random_buffer((uint8_t *) &value, sizeof(value));
    return value;
}


/// @brief Test the connect token
static int pomelo_test_connect_token(void) {
    int ret = 0;

    // Setup a connect token
    token.protocol_id = random_u64();
    token.create_timestamp = random_u64();
    token.expire_timestamp = random_u64();
    pomelo_random_buffer(
        token.connect_token_nonce,
        sizeof(token.connect_token_nonce)
    );
    token.client_id = random_i64();
    token.timeout = random_i32();
    // At least 1 address: a connect token with 0 server addresses is invalid
    // and is rejected on decode (crypto/token.c naddr==0 guard), so 0 here
    // would make this roundtrip test fail ~1/MAX of the time.
    token.naddresses = 1 + random_u16() % POMELO_CONNECT_TOKEN_MAX_ADDRESSES;
    for (int i = 0; i < token.naddresses; i++) {
        token.addresses[i].type = POMELO_ADDRESS_IPV4;
        token.addresses[i].ip.v4[0] = random_u8();
        token.addresses[i].ip.v4[1] = random_u8();
        token.addresses[i].ip.v4[2] = random_u8();
        token.addresses[i].ip.v4[3] = random_u8();
        token.addresses[i].port = random_u16();
    }
    
    pomelo_random_buffer(
        token.client_to_server_key,
        sizeof(token.client_to_server_key)
    );
    pomelo_random_buffer(
        token.server_to_client_key,
        sizeof(token.server_to_client_key)
    );
    pomelo_random_buffer(
        token.user_data,
        sizeof(token.user_data)
    );

    // Encode connect token to buffer
    ret = pomelo_connect_token_encode(
        connect_token,
        &token,
        crypto_ctx->private_key
    );
    pomelo_check(ret == 0);
 
    // Decode connect token from public part
    ret = pomelo_connect_token_decode_public(connect_token, &decoded_token);
    pomelo_check(ret == 0);

    // Decode private part
    ret = pomelo_connect_token_decode_private(
        connect_token + POMELO_CONNECT_TOKEN_PRIVATE_OFFSET,
        &decoded_token,
        crypto_ctx->private_key
    );
    pomelo_check(ret == 0);

    // Check the values
    pomelo_check(token.protocol_id == decoded_token.protocol_id);
    pomelo_check(token.create_timestamp == decoded_token.create_timestamp);
    pomelo_check(token.expire_timestamp == decoded_token.expire_timestamp);

    // Check the nonce
    ret = memcmp(
        token.connect_token_nonce,
        decoded_token.connect_token_nonce,
        sizeof(token.connect_token_nonce)
    );
    pomelo_check(ret == 0);
    pomelo_check(token.timeout == decoded_token.timeout);

    // Addresses
    pomelo_check(token.naddresses == decoded_token.naddresses);
    for (int i = 0; i < token.naddresses; i++) {
        bool cmp_ret = pomelo_address_compare(
            &token.addresses[i],
            &decoded_token.addresses[i]
        );
        pomelo_check(cmp_ret);
    }

    // Client to server key
    ret = memcmp(
        token.client_to_server_key,
        decoded_token.client_to_server_key,
        sizeof(token.client_to_server_key)
    );
    pomelo_check(ret == 0);

    // Server to client key
    ret = memcmp(
        token.server_to_client_key,
        decoded_token.server_to_client_key,
        sizeof(token.server_to_client_key)
    );
    pomelo_check(ret == 0);

    pomelo_check(token.client_id == decoded_token.client_id);

    // User data
    ret = memcmp(
        token.user_data,
        decoded_token.user_data,
        sizeof(token.user_data)
    );
    pomelo_check(ret == 0);

    return 0;
}


/// @brief Test codec request packet
static int pomelo_test_request_packet(void) {
    pomelo_track_function();
    int ret = 0;

    // Encode connect token again
    ret = pomelo_connect_token_encode(
        connect_token,
        &token,
        crypto_ctx->private_key
    );
    pomelo_check(ret == 0);

    // Acquire new packet request
    pomelo_protocol_packet_request_info_t info = {
        .protocol_id = token.protocol_id,
        .expire_timestamp = token.expire_timestamp,
        .connect_token_nonce = token.connect_token_nonce,
        .encrypted_connect_token =
            (connect_token + POMELO_CONNECT_TOKEN_PRIVATE_OFFSET)
    };
    pomelo_protocol_packet_request_t * packet = pomelo_pool_acquire(
        protocol_ctx->packet_pools[POMELO_PROTOCOL_PACKET_REQUEST],
        &info
    );
    pomelo_check(packet != NULL);

    // Acquire new buffer to store encrypted packet
    pomelo_buffer_t * buffer = pomelo_buffer_context_acquire(buffer_ctx);
    pomelo_check(buffer != NULL);
    
    pomelo_buffer_view_t view;
    view.buffer = buffer;
    view.length = 0;
    view.offset = 0;

    // Set private key before encoding
    ret = encode_and_encrypt_packet(&packet->base, &view);
    pomelo_check(ret == 0);

    // Release the packet
    pomelo_protocol_context_release_packet(protocol_ctx, &packet->base);

    // Decode the header
    pomelo_protocol_packet_header_t header = { 0 };
    ret = pomelo_protocol_packet_header_decode(&header, &view);
    pomelo_check(ret == 0);
    pomelo_check(header.type == POMELO_PROTOCOL_PACKET_REQUEST);

    // Acquire new packet request for decoding
    packet = pomelo_pool_acquire(
        protocol_ctx->packet_pools[POMELO_PROTOCOL_PACKET_REQUEST],
        NULL
    );
    pomelo_check(packet != NULL);

    // Decode the packet
    ret = decrypt_and_decode_packet(&packet->base, &view, &header);
    pomelo_check(ret == 0);
    
    pomelo_check(packet->protocol_id == token.protocol_id);
    pomelo_check(packet->expire_timestamp == token.expire_timestamp);
    ret = memcmp(
        packet->connect_token_nonce,
        token.connect_token_nonce,
        sizeof(token.connect_token_nonce)
    );
    pomelo_check(ret == 0);
    pomelo_check(packet->token_data.token.client_id == token.client_id);

    // Release the packet
    pomelo_protocol_context_release_packet(protocol_ctx, &packet->base);

    // Unref the buffer
    pomelo_buffer_unref(buffer);
    
    return 0;
}


/// @brief Test code challenge-response packet
static int pomelo_test_challenge_response_packet(void) {
    pomelo_track_function();
    uint64_t sequence = random_u64();
    uint64_t token_sequence = random_u64();

    pomelo_protocol_packet_challenge_info_t info_challenge = {
        .sequence = sequence,
        .token_sequence = token_sequence,
        .client_id = token.client_id,
        .user_data = token.user_data
    };
    pomelo_protocol_packet_challenge_t * packet_challenge = pomelo_pool_acquire(
        protocol_ctx->packet_pools[POMELO_PROTOCOL_PACKET_CHALLENGE],
        &info_challenge
    );
    pomelo_check(packet_challenge != NULL);

    // Acquire new buffer to store encrypted packet
    pomelo_buffer_t * buffer = pomelo_buffer_context_acquire(buffer_ctx);
    pomelo_check(buffer != NULL);
    
    pomelo_buffer_view_t view;
    view.buffer = buffer;
    view.length = 0;
    view.offset = 0;

    // Encode the packet challenge
    int ret = encode_and_encrypt_packet(&packet_challenge->base, &view);
    pomelo_check(ret == 0);

    // Release the packet
    pomelo_protocol_context_release_packet(
        protocol_ctx,
        &packet_challenge->base
    );

    // Decode the packet header
    pomelo_protocol_packet_header_t header = { 0 };
    ret = pomelo_protocol_packet_header_decode(&header, &view);
    pomelo_check(ret == 0);

    pomelo_check(header.type == POMELO_PROTOCOL_PACKET_CHALLENGE);
    pomelo_check(header.sequence == sequence);

    // Acquire new packet challenge for decoding
    packet_challenge = pomelo_pool_acquire(
        protocol_ctx->packet_pools[POMELO_PROTOCOL_PACKET_CHALLENGE],
        NULL
    );
    pomelo_check(packet_challenge != NULL);

    // Decode the packet challenge
    ret = decrypt_and_decode_packet(&packet_challenge->base, &view, &header);
    pomelo_check(ret == 0);

    // Build the packet response
    pomelo_protocol_packet_response_info_t info_response = {
        .sequence = ++sequence,
        .token_sequence = packet_challenge->token_sequence,
        .encrypted_challenge_token = packet_challenge->challenge_data.encrypted
    };
    pomelo_protocol_packet_response_t * packet_response = pomelo_pool_acquire(
        protocol_ctx->packet_pools[POMELO_PROTOCOL_PACKET_RESPONSE],
        &info_response
    );
    pomelo_check(packet_response != NULL);

    // Reset view before encoding
    view.buffer = buffer;
    view.length = 0;
    view.offset = 0;

    // Encode the packet response
    ret = encode_and_encrypt_packet(&packet_response->base, &view);
    pomelo_check(ret == 0);

    // Release the packets
    pomelo_protocol_context_release_packet(
        protocol_ctx, &packet_response->base
    );
    pomelo_protocol_context_release_packet(
        protocol_ctx, &packet_challenge->base
    );

    // Decode the packet header
    memset(&header, 0, sizeof(pomelo_protocol_packet_header_t));
    ret = pomelo_protocol_packet_header_decode(&header, &view);
    pomelo_check(ret == 0);
    pomelo_check(header.type == POMELO_PROTOCOL_PACKET_RESPONSE);
    pomelo_check(header.sequence == sequence);

    // Acquire new packet response for decoding
    packet_response = pomelo_pool_acquire(
        protocol_ctx->packet_pools[POMELO_PROTOCOL_PACKET_RESPONSE],
        NULL
    );
    pomelo_check(packet_response != NULL);

    // Decode the packet challenge
    ret = decrypt_and_decode_packet(&packet_response->base, &view, &header);
    pomelo_check(ret == 0);

    // Check the data of response
    pomelo_check(
        packet_response->challenge_data.token.client_id == token.client_id
    );
    ret = memcmp(
        packet_response->challenge_data.token.user_data,
        token.user_data,
        sizeof(token.user_data)
    );
    pomelo_check(ret == 0);

    // Release the packet
    pomelo_protocol_context_release_packet(
        protocol_ctx,
        &packet_response->base
    );

    // Unref the buffer
    pomelo_buffer_unref(buffer);
    return 0;
}


static int pomelo_test_denied_packet(void) {
    pomelo_track_function();
    uint64_t sequence = random_u64();

    // Acquire new buffer to store encrypted packet
    pomelo_buffer_t * buffer = pomelo_buffer_context_acquire(buffer_ctx);
    pomelo_check(buffer != NULL);
    
    pomelo_buffer_view_t view;
    view.buffer = buffer;
    view.length = 0;
    view.offset = 0;

    // Build the packet denied
    pomelo_protocol_packet_denied_info_t info = {
        .sequence = sequence
    };
    pomelo_protocol_packet_denied_t * packet = pomelo_pool_acquire(
        protocol_ctx->packet_pools[POMELO_PROTOCOL_PACKET_DENIED],
        &info
    );
    pomelo_check(packet != NULL);

    // Encode the packet denied
    int ret = encode_and_encrypt_packet(&packet->base, &view);
    pomelo_check(ret == 0);

    // Release the packet
    pomelo_protocol_context_release_packet(protocol_ctx, &packet->base);

    // Decode the packet header
    pomelo_protocol_packet_header_t header;
    ret = pomelo_protocol_packet_header_decode(&header, &view);
    pomelo_check(ret == 0);

    pomelo_check(header.type == POMELO_PROTOCOL_PACKET_DENIED);
    pomelo_check(header.sequence == sequence);

    // Acquire new packet denied for decoding
    packet = pomelo_pool_acquire(
        protocol_ctx->packet_pools[POMELO_PROTOCOL_PACKET_DENIED],
        NULL
    );
    pomelo_check(packet != NULL);

    // Decode the packet
    ret = decrypt_and_decode_packet(&packet->base, &view, &header);
    pomelo_check(ret == 0);
    pomelo_check(packet->base.sequence == sequence);

    // Release the packet
    pomelo_protocol_context_release_packet(protocol_ctx, &packet->base);

    // Unref the buffer
    pomelo_buffer_unref(buffer);

    return 0;
}


static int pomelo_test_keep_alive_packet(void) {
    pomelo_track_function();
    uint64_t sequence = random_u64();

    // Acquire new buffer to store encrypted packet
    pomelo_buffer_t * buffer = pomelo_buffer_context_acquire(buffer_ctx);
    pomelo_check(buffer != NULL);
    
    pomelo_buffer_view_t view;
    view.buffer = buffer;
    view.length = 0;
    view.offset = 0;

    // Build the packet keep alive
    pomelo_protocol_packet_keep_alive_info_t info = {
        .sequence = ++sequence,
        .client_id = token.client_id
    };
    pomelo_protocol_packet_keep_alive_t * packet = pomelo_pool_acquire(
        protocol_ctx->packet_pools[POMELO_PROTOCOL_PACKET_KEEP_ALIVE],
        &info
    );
    pomelo_check(packet != NULL);
    
    // Encode the packet keep alive
    int ret = encode_and_encrypt_packet(&packet->base, &view);
    pomelo_check(ret == 0);

    // Release the packet
    pomelo_protocol_context_release_packet(protocol_ctx, &packet->base);

    // Decode the packet header
    pomelo_protocol_packet_header_t header = { 0 };
    ret = pomelo_protocol_packet_header_decode(&header, &view);
    pomelo_check(ret == 0);
    pomelo_check(header.type == POMELO_PROTOCOL_PACKET_KEEP_ALIVE);
    pomelo_check(header.sequence == sequence);

    // Acquire new packet keep alive for decoding
    packet = pomelo_pool_acquire(
        protocol_ctx->packet_pools[POMELO_PROTOCOL_PACKET_KEEP_ALIVE],
        NULL
    );
    pomelo_check(packet != NULL);

    // Decode the packet
    ret = decrypt_and_decode_packet(&packet->base, &view, &header);
    pomelo_check(ret == 0);

    // Release the packet
    pomelo_protocol_context_release_packet(protocol_ctx, &packet->base);

    // Unref the buffer
    pomelo_buffer_unref(buffer);
    return 0;
}


static int pomelo_test_payload_packet(void) {
    pomelo_track_function();
    uint64_t sequence = random_u64();

    // Acquire new buffer to store encrypted packet
    pomelo_buffer_t * buffer = pomelo_buffer_context_acquire(buffer_ctx);
    pomelo_check(buffer != NULL);
    
    pomelo_buffer_t * content = pomelo_buffer_context_acquire(buffer_ctx);
    pomelo_check(content != NULL);

    // Write data to payload buffer
    pomelo_payload_t payload;
    payload.data = content->data;
    payload.capacity = content->capacity;
    payload.position = 0;

    int32_t v1 = random_i32();
    uint64_t v2 = random_u64();
    pomelo_payload_write_int32(&payload, v1);
    pomelo_payload_write_uint64(&payload, v2);

    // Build payload view
    pomelo_buffer_view_t view;
    view.buffer = content;
    view.length = payload.position;
    view.offset = 0;

    // Build the packet payload
    pomelo_protocol_packet_payload_info_t info = {
        .sequence = ++sequence,
        .nviews = 1,
        .views = &view
    };
    pomelo_protocol_packet_payload_t * packet = pomelo_pool_acquire(
        protocol_ctx->packet_pools[POMELO_PROTOCOL_PACKET_PAYLOAD],
        &info
    );
    pomelo_check(packet != NULL);

    // Change the view to output buffer
    view.buffer = buffer;
    view.length = 0;
    view.offset = 0;

    // Encode and encrypt the packet payload to buffer
    int ret = encode_and_encrypt_packet(&packet->base, &view);
    pomelo_check(ret == 0);

    // Release the packet
    pomelo_protocol_context_release_packet(protocol_ctx, &packet->base);

    // Decode the packet header
    pomelo_protocol_packet_header_t header = { 0 };
    ret = pomelo_protocol_packet_header_decode(&header, &view);
    pomelo_check(ret == 0);
    pomelo_check(header.type == POMELO_PROTOCOL_PACKET_PAYLOAD);
    pomelo_check(header.sequence == sequence);

    // Clear packet payload and attach the buffer again
    info.sequence = header.sequence;
    info.nviews = 0;
    info.views = NULL;
    packet = pomelo_pool_acquire(
        protocol_ctx->packet_pools[POMELO_PROTOCOL_PACKET_PAYLOAD],
        &info
    );
    pomelo_check(packet != NULL);

    // Decode the packet
    ret = decrypt_and_decode_packet(&packet->base, &view, &header);
    pomelo_check(ret == 0);

    // No need to update the payload offset here
    int32_t read_v1 = 0;
    uint64_t read_v2 = 0;

    payload.data = view.buffer->data + view.offset;
    payload.capacity = view.buffer->capacity - view.offset;
    payload.position = 0;

    ret = pomelo_payload_read_int32(&payload, &read_v1);
    pomelo_check(ret == 0);
    pomelo_check(read_v1 == v1);

    ret = pomelo_payload_read_uint64(&payload, &read_v2);
    pomelo_check(read_v2 == v2);

    // Release the packet
    pomelo_protocol_context_release_packet(protocol_ctx, &packet->base);

    // Unref the buffers
    pomelo_buffer_unref(buffer);
    pomelo_buffer_unref(content);
    return 0;
}


static int pomelo_test_disconnect_packet(void) {
    pomelo_track_function();
    uint64_t sequence = random_u64();

    // Acquire new buffer to store encrypted packet
    pomelo_buffer_t * buffer = pomelo_buffer_context_acquire(buffer_ctx);
    pomelo_check(buffer != NULL);
    
    pomelo_buffer_view_t view;
    view.buffer = buffer;
    view.length = 0;
    view.offset = 0;

    // Build the packet denied
    pomelo_protocol_packet_denied_info_t info = {
        .sequence = sequence
    };
    pomelo_protocol_packet_denied_t * packet = pomelo_pool_acquire(
        protocol_ctx->packet_pools[POMELO_PROTOCOL_PACKET_DISCONNECT],
        &info
    );
    pomelo_check(packet != NULL);

    // Encode the packet denied
    int ret = encode_and_encrypt_packet(&packet->base, &view);
    pomelo_check(ret == 0);

    // Release the packet
    pomelo_protocol_context_release_packet(protocol_ctx, &packet->base);

    // Decode the packet header
    pomelo_protocol_packet_header_t header = { 0 };
    ret = pomelo_protocol_packet_header_decode(&header, &view);
    pomelo_check(ret == 0);

    pomelo_check(header.type == POMELO_PROTOCOL_PACKET_DISCONNECT);
    pomelo_check(header.sequence == sequence);

    // Acquire new packet denied for decoding
    packet = pomelo_pool_acquire(
        protocol_ctx->packet_pools[POMELO_PROTOCOL_PACKET_DISCONNECT],
        NULL
    );
    pomelo_check(packet != NULL);

    // Decode the packet
    ret = decrypt_and_decode_packet(&packet->base, &view, &header);
    pomelo_check(ret == 0);
    pomelo_check(packet->base.sequence == sequence);

    // Release the packet
    pomelo_protocol_context_release_packet(protocol_ctx, &packet->base);

    // Unref the buffer
    pomelo_buffer_unref(buffer);

    return 0;
}


/// @brief Regression test for the body-length underflow guard in
/// pomelo_protocol_packet_validate_body_length (src/protocol/packet.c).
/// An encrypted (non-REQUEST) packet body must be at least POMELO_HMAC_BYTES
/// long to hold the AEAD auth tag; a shorter body would underflow when the tag
/// size is subtracted before decrypt. The guard must reject it (and the real
/// decrypt path must fail-closed instead of reading past the buffer).
static int test_decode_rejects_short_body(void) {
    pomelo_track_function();

    // --- Part 1: exercise the guard function directly (it is the function the
    // production decode path -- pomelo_protocol_socket_validate_incoming -- uses
    // right after decoding the header). Every body length below the auth tag
    // size must be rejected for an encrypted packet, for every packet type.
    for (size_t len = 0; len < POMELO_HMAC_BYTES; len++) {
        pomelo_check(pomelo_protocol_packet_validate_body_length(
            POMELO_PROTOCOL_PACKET_PAYLOAD, len, true) == false);
        pomelo_check(pomelo_protocol_packet_validate_body_length(
            POMELO_PROTOCOL_PACKET_KEEP_ALIVE, len, true) == false);
        pomelo_check(pomelo_protocol_packet_validate_body_length(
            POMELO_PROTOCOL_PACKET_DISCONNECT, len, true) == false);
        pomelo_check(pomelo_protocol_packet_validate_body_length(
            POMELO_PROTOCOL_PACKET_DENIED, len, true) == false);
    }

    // A tag-only body (exactly POMELO_HMAC_BYTES) is the smallest a zero-length
    // body type accepts: proves the rejection boundary sits at the tag size.
    pomelo_check(pomelo_protocol_packet_validate_body_length(
        POMELO_PROTOCOL_PACKET_DISCONNECT, POMELO_HMAC_BYTES, true) == true);

    // --- Part 2: drive a truncated encrypted body through the REAL decrypt +
    // decode path and confirm it fails (< 0) without crashing / reading OOB.
    pomelo_buffer_t * buffer = pomelo_buffer_context_acquire(buffer_ctx);
    pomelo_check(buffer != NULL);

    pomelo_buffer_view_t view;
    view.buffer = buffer;
    view.length = 0;
    view.offset = 0;

    // Build & encrypt a valid DISCONNECT so there is a real header on the wire.
    uint64_t sequence = random_u64();
    pomelo_protocol_packet_denied_info_t info = { .sequence = sequence };
    pomelo_protocol_packet_denied_t * packet = pomelo_pool_acquire(
        protocol_ctx->packet_pools[POMELO_PROTOCOL_PACKET_DISCONNECT],
        &info
    );
    pomelo_check(packet != NULL);
    pomelo_check(encode_and_encrypt_packet(&packet->base, &view) == 0);
    pomelo_protocol_context_release_packet(protocol_ctx, &packet->base);

    // Decode the header off the wire.
    pomelo_protocol_packet_header_t header = { 0 };
    pomelo_check(pomelo_protocol_packet_header_decode(&header, &view) == 0);
    pomelo_check(header.type == POMELO_PROTOCOL_PACKET_DISCONNECT);

    // Truncate the body below POMELO_HMAC_BYTES to simulate a malicious /
    // corrupted short packet, then run the real decode path.
    view.length = 4; // < POMELO_HMAC_BYTES

    packet = pomelo_pool_acquire(
        protocol_ctx->packet_pools[POMELO_PROTOCOL_PACKET_DISCONNECT],
        NULL
    );
    pomelo_check(packet != NULL);

    int ret = decrypt_and_decode_packet(&packet->base, &view, &header);
    pomelo_check(ret < 0); // rejected: no underflow, no out-of-bounds read

    pomelo_protocol_context_release_packet(protocol_ctx, &packet->base);
    pomelo_buffer_unref(buffer);
    return 0;
}


int main(void) {
    printf("Packet test\n");
    pomelo_check(pomelo_crypto_init() == 0);

    allocator = pomelo_allocator_default();
    uint64_t alloc_bytes = pomelo_allocator_allocated_bytes(allocator);

    // Create buffer context
    pomelo_buffer_context_root_options_t buffer_ctx_opts = {
        .allocator = allocator,
        .buffer_capacity = POMELO_BUFFER_CAPACITY,
    };
    buffer_ctx = pomelo_buffer_context_root_create(&buffer_ctx_opts);
    pomelo_check(buffer_ctx != NULL);

    // Create protocol context
    pomelo_protocol_context_options_t protocol_ctx_opts = {
        .allocator = allocator,
        .buffer_context = buffer_ctx,
        .payload_capacity = POMELO_PACKET_BODY_CAPACITY
    };
    protocol_ctx = pomelo_protocol_context_create(&protocol_ctx_opts);
    pomelo_check(protocol_ctx != NULL);

    // Acquire a crypto context
    crypto_ctx = pomelo_protocol_context_acquire_crypto_context(protocol_ctx);
    pomelo_check(crypto_ctx != NULL);

    // Random values
    pomelo_random_buffer(
        crypto_ctx->packet_decrypt_key,
        sizeof(crypto_ctx->packet_decrypt_key)
    );
    memcpy(
        crypto_ctx->packet_encrypt_key,
        crypto_ctx->packet_decrypt_key,
        sizeof(crypto_ctx->packet_encrypt_key)
    );

    pomelo_random_buffer(
        crypto_ctx->challenge_key,
        sizeof(crypto_ctx->challenge_key)
    );

    pomelo_random_buffer(
        crypto_ctx->private_key,
        sizeof(crypto_ctx->private_key)
    );
    
    crypto_ctx->protocol_id = random_u64();

    // Run tests
    pomelo_check(pomelo_test_connect_token() == 0);
    pomelo_check(pomelo_test_request_packet() == 0);
    pomelo_check(pomelo_test_challenge_response_packet() == 0);
    pomelo_check(pomelo_test_keep_alive_packet() == 0);
    pomelo_check(pomelo_test_payload_packet() == 0);
    pomelo_check(pomelo_test_disconnect_packet() == 0);
    pomelo_check(pomelo_test_denied_packet() == 0);
    pomelo_check(test_decode_rejects_short_body() == 0);

    // Release resources
    pomelo_protocol_context_release_crypto_context(protocol_ctx, crypto_ctx);
    pomelo_protocol_context_destroy(protocol_ctx);
    pomelo_buffer_context_destroy(buffer_ctx);

    // Check memleak
    pomelo_check(alloc_bytes == pomelo_allocator_allocated_bytes(allocator));

    printf("*** All packet tests passed ***\n");
    return 0;
}


void pomelo_protocol_socket_on_connect_result(
    pomelo_protocol_socket_t * socket,
    int result
) {
    (void) socket;
    (void) result;
}


void pomelo_protocol_socket_on_disconnect(
    pomelo_protocol_socket_t * socket
) {
    (void) socket;
}


void pomelo_protocol_socket_on_received(
    pomelo_protocol_socket_t * socket,
    pomelo_protocol_peer_t * peer,
    pomelo_buffer_view_t * view
) {
    (void) socket;
    (void) peer;
    (void) view;
}


void pomelo_protocol_socket_on_connected(
    pomelo_protocol_socket_t * socket,
    pomelo_protocol_peer_t * peer
) {
    (void) socket;
    (void) peer;
}


void pomelo_protocol_socket_on_disconnected(
    pomelo_protocol_socket_t * socket,
    pomelo_protocol_peer_t * peer
) {
    (void) socket;
    (void) peer;
}
