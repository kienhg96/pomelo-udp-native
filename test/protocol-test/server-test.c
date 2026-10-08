#include "pomelo-test.h"
#include "pomelo/random.h"
#include "pomelo/platforms/platform-uv.h"
#include "protocol/client.h"
#include "protocol/context.h"
#include "crypto/crypto.h"
#include "protocol/packet.h"
#include "adapter-simulator.h"
#include "statistic-check/statistic-check.h"

/*
    Protocol server test:
        - Simulator sends request packet
        - Server responses challenge packet
        - Simulator check challenge packet and replies response packet
        - Server replies a keep alive packet
        - Simulator replies a keep alive packet as well
        - Server dispatches connected event
        - Server prepares and sends a payload to client
        - Simulator received a payload, check the content of payload
*/


// Constants
#define SERVER_ADDRESS "127.0.0.1:8888"
#define CLIENT_ADDRESS "127.0.0.1:8889"
#define MAX_CLIENTS 10
#define CONNECT_TIMEOUT 1 // seconds
#define TOKEN_EXPIRE 3600 // seconds


// Allocator, platform & context
static pomelo_allocator_t * allocator;
static pomelo_platform_t * platform;
static pomelo_buffer_context_t * buffer_ctx;
static pomelo_protocol_context_t * protocol_ctx;
static uv_loop_t uv_loop;
static pomelo_sequencer_t sequencer;

// Adapter
static pomelo_adapter_t * adapter_client;

// Socket
static pomelo_protocol_socket_t * server;

// Token
static uint8_t connect_token[POMELO_CONNECT_TOKEN_BYTES];

// Codec
static pomelo_protocol_crypto_context_t crypto_ctx;

// Temp variables
static pomelo_connect_token_t token;
static pomelo_platform_uv_options_t platform_options;
static pomelo_protocol_context_options_t protocol_ctx_opts;
static pomelo_buffer_context_root_options_t buffer_ctx_opts;
static pomelo_protocol_server_options_t server_options;


// Global variables
static uint64_t sequence;

static pomelo_address_t address;

static uint64_t protocol_id;
static int64_t client_id;
static int32_t sample_v1;
static uint64_t sample_v2;

static volatile int server_completed = 0;


/// @brief Encode, encrypt and dispatch the packet to client
static void deliver_outgoing_packet(pomelo_protocol_packet_t * packet) {
    int ret = 0;

    // Acquire new buffer for sending
    pomelo_buffer_t * buffer = pomelo_buffer_context_acquire(buffer_ctx);
    pomelo_check(buffer != NULL);

    pomelo_buffer_view_t view;
    view.buffer = buffer;
    view.offset = 0;
    view.length = 0;

    // Encode & encrypt packet
    pomelo_protocol_packet_header_t header;
    pomelo_protocol_packet_header_init(&header, packet);

    ret = pomelo_protocol_packet_header_encode(&header, &view);
    pomelo_check(ret == 0);

    // Make the view of packet body
    pomelo_buffer_view_t body_view;
    body_view.buffer = view.buffer;
    body_view.offset = view.offset + view.length; // Skip header
    body_view.length = 0;

    ret = pomelo_protocol_packet_encode(packet, &crypto_ctx, &body_view);
    pomelo_check(ret == 0);

    ret = pomelo_protocol_crypto_context_encrypt_packet(
        &crypto_ctx, &body_view, &header
    );
    pomelo_check(ret == 0);

    // Update the original view
    view.length += body_view.length;

    // Dispatch message
    pomelo_adapter_recv(server->adapter, &address, &view);
    pomelo_buffer_unref(buffer);
}


/// @brief Decrypt and decode incoming packet
static void process_incoming_packet(
    pomelo_protocol_packet_t * packet,
    pomelo_protocol_packet_header_t * header,
    pomelo_buffer_view_t * body_view
) {
    int ret = 0;

    // Update header value
    packet->sequence = header->sequence;

    // Decrypt body
    ret = pomelo_protocol_crypto_context_decrypt_packet(
        &crypto_ctx, body_view, header
    );
    pomelo_check(ret == 0);

    // Decode body
    ret = pomelo_protocol_packet_decode(packet, &crypto_ctx, body_view);
    pomelo_check(ret == 0);
}


static void process_challenge_packet(
    pomelo_protocol_packet_header_t * header,
    pomelo_buffer_view_t * body_view
) {
    pomelo_track_function();

    // Acquire a challenge packet for decoding
    pomelo_protocol_packet_challenge_t * packet_challenge = pomelo_pool_acquire(
        protocol_ctx->packet_pools[POMELO_PROTOCOL_PACKET_CHALLENGE],
        NULL
    );
    pomelo_check(packet_challenge != NULL);
    process_incoming_packet(&packet_challenge->base, header, body_view);

    // Acquire a response packet for sending
    pomelo_protocol_packet_response_info_t info = {
        .sequence = ++sequence,
        .token_sequence = packet_challenge->token_sequence,
        .encrypted_challenge_token = packet_challenge->challenge_data.encrypted
    };
    pomelo_protocol_packet_response_t * packet_response = pomelo_pool_acquire(
        protocol_ctx->packet_pools[POMELO_PROTOCOL_PACKET_RESPONSE],
        &info
    );
    pomelo_check(packet_response != NULL);

    deliver_outgoing_packet(&packet_response->base);

    // Release packets
    pomelo_pool_release(
        protocol_ctx->packet_pools[POMELO_PROTOCOL_PACKET_CHALLENGE],
        packet_challenge
    );

    pomelo_pool_release(
        protocol_ctx->packet_pools[POMELO_PROTOCOL_PACKET_RESPONSE],
        packet_response
    );
}


static void process_keep_alive_packet(
    pomelo_protocol_packet_header_t * header,
    pomelo_buffer_view_t * body_view
) {
    pomelo_track_function();
    // Acquire new keep alive packet for decoding
    pomelo_protocol_packet_keep_alive_t * packet = pomelo_pool_acquire(
        protocol_ctx->packet_pools[POMELO_PROTOCOL_PACKET_KEEP_ALIVE],
        NULL
    );
    pomelo_check(packet != NULL);
    process_incoming_packet(&packet->base, header, body_view);

    // Reply keep alive packet
    pomelo_protocol_packet_keep_alive_info_t info = {
        .sequence = ++sequence,
        .client_id = token.client_id
    };
    pomelo_protocol_packet_keep_alive_t * packet_reply = pomelo_pool_acquire(
        protocol_ctx->packet_pools[POMELO_PROTOCOL_PACKET_KEEP_ALIVE],
        &info
    );
    pomelo_check(packet_reply != NULL);

    deliver_outgoing_packet(&packet_reply->base);

    // Release packets
    pomelo_pool_release(
        protocol_ctx->packet_pools[POMELO_PROTOCOL_PACKET_KEEP_ALIVE],
        packet
    );

    pomelo_pool_release(
        protocol_ctx->packet_pools[POMELO_PROTOCOL_PACKET_KEEP_ALIVE],
        packet_reply
    );
}


static void process_payload_packet(
    pomelo_protocol_packet_header_t * header,
    pomelo_buffer_view_t * body_view
) {
    pomelo_track_function();

    pomelo_pool_t * pool =
        protocol_ctx->packet_pools[POMELO_PROTOCOL_PACKET_PAYLOAD];

    // Process incoming packet
    pomelo_protocol_packet_payload_info_t info = {
        .sequence = ++sequence,
        .nviews = 0,
        .views = NULL
    };
    pomelo_protocol_packet_payload_t * packet =
        pomelo_pool_acquire(pool, &info);
    pomelo_check(packet != NULL);

    process_incoming_packet(&packet->base, header, body_view);
    pomelo_check(packet->nviews == 1);

    // Keep the view and buffer before releasing the packet
    pomelo_buffer_view_t view = packet->views[0];
    pomelo_buffer_ref(view.buffer);

    // Release the packet
    pomelo_pool_release(pool, packet);

    // Rebuild the payload packet again for sending
    info.sequence = ++sequence;
    info.nviews = 1;
    info.views = &view;
    packet = pomelo_pool_acquire(pool, &info);
    pomelo_check(packet != NULL);

    // Reply the packet
    deliver_outgoing_packet(&packet->base);

    // Then release the packet
    pomelo_pool_release(pool, packet);

    // Unref the buffer
    pomelo_buffer_unref(view.buffer);
}


/// Handle send requests from client 
static void send_handler(
    pomelo_address_t * address,
    pomelo_buffer_view_t * view
) {
    (void) address;

    pomelo_protocol_packet_header_t header;
    int ret = pomelo_protocol_packet_header_decode(&header, view);
    pomelo_check(ret == 0);

    switch (header.type) {
        case POMELO_PROTOCOL_PACKET_CHALLENGE:
            process_challenge_packet(&header, view);
            break;

        case POMELO_PROTOCOL_PACKET_KEEP_ALIVE:
            process_keep_alive_packet(&header, view);
            break;

        case POMELO_PROTOCOL_PACKET_PAYLOAD:
            process_payload_packet(&header, view);
            break;

        default:
            break;
    }
}


/// @brief Send request packet
static void send_request_packet(void * unused) {
    printf("[i] Start sending request packet...\n");
    (void) unused;

    // Acquire a request packet for sending
    pomelo_protocol_packet_request_info_t info = {
        .sequence = 0,
        .protocol_id = token.protocol_id,
        .expire_timestamp = token.expire_timestamp,
        .connect_token_nonce = token.connect_token_nonce,
        .encrypted_connect_token = 
            (connect_token + POMELO_CONNECT_TOKEN_PRIVATE_OFFSET)
    };
    pomelo_protocol_packet_request_t * packet_request = pomelo_pool_acquire(
        protocol_ctx->packet_pools[POMELO_PROTOCOL_PACKET_REQUEST],
        &info
    );
    pomelo_check(packet_request != NULL);
    deliver_outgoing_packet(&packet_request->base);

    // Release the packet
    pomelo_pool_release(
        protocol_ctx->packet_pools[POMELO_PROTOCOL_PACKET_REQUEST],
        packet_request
    );
}


void pomelo_protocol_socket_on_connected(
    pomelo_protocol_socket_t * socket,
    pomelo_protocol_peer_t * peer
) {
    (void) socket;
    pomelo_track_function();

    int ret = 0;

    // Prepare a buffer to send
    pomelo_buffer_t * buffer = pomelo_buffer_context_acquire(buffer_ctx);
    pomelo_check(buffer != NULL);

    pomelo_payload_t payload;
    payload.data = buffer->data;
    payload.capacity = buffer->capacity;
    payload.position = 0;

    ret = pomelo_payload_write_int32(&payload, sample_v1);
    pomelo_check(ret == 0);
    ret = pomelo_payload_write_uint64(&payload, sample_v2);
    pomelo_check(ret == 0);

    pomelo_buffer_view_t view;
    view.buffer = buffer;
    view.offset = 0;
    view.length = payload.position;

    ret = pomelo_protocol_peer_send(peer, &view, 1);
    pomelo_check(ret == 0);

    // Unref buffer after sending
    pomelo_buffer_unref(buffer);
}


void pomelo_protocol_socket_on_disconnected(
    pomelo_protocol_socket_t * socket,
    pomelo_protocol_peer_t * peer
) {
    (void) peer;
    pomelo_track_function();

    printf("[i] Client disconnected.\n");
    pomelo_protocol_socket_stop(socket);
}


void pomelo_protocol_socket_on_received(
    pomelo_protocol_socket_t * socket,
    pomelo_protocol_peer_t * peer,
    pomelo_buffer_view_t * view
) {
    (void) socket;
    pomelo_track_function();

    int ret = 0;

    pomelo_payload_t payload;
    payload.data = view->buffer->data + view->offset;
    payload.capacity = view->length;
    payload.position = 0;

    int32_t v1 = 0;
    uint64_t v2 = 0;

    ret = pomelo_payload_read_int32(&payload, &v1);
    pomelo_check(ret == 0);
    pomelo_check(v1 == sample_v1);

    ret = pomelo_payload_read_uint64(&payload, &v2);
    pomelo_check(ret == 0);
    pomelo_check(v2 == sample_v2);

    server_completed = 1;

    // Disconnect peer
    printf("[i] Disconnecting peer...\n");
    pomelo_protocol_peer_disconnect(peer);
}


void pomelo_protocol_socket_on_connect_result(
    pomelo_protocol_socket_t * socket,
    pomelo_protocol_connect_result result
) {
    (void) socket;
    pomelo_track_function();
    printf("[i] Connect result %d\n", result);
}


int main(void) {
    printf("Test protocol server.\n");
    if (pomelo_crypto_init() < 0) {
        printf("Failed to initialize codec\n");
        return -1;
    }

    int ret = 0;
    allocator = pomelo_allocator_default();

    uint64_t alloc_bytes = pomelo_allocator_allocated_bytes(allocator);

    // Initialize UV loop
    uv_loop_init(&uv_loop);

    // Create platform
    memset(&platform_options, 0, sizeof(pomelo_platform_uv_options_t));
    platform_options.allocator = allocator;
    platform_options.uv_loop = &uv_loop;
    platform = pomelo_platform_uv_create(&platform_options);
    pomelo_check(platform != NULL);

    // Initialize sequencer
    pomelo_sequencer_init(&sequencer);

    // Create buffer context
    memset(&buffer_ctx_opts, 0, sizeof(pomelo_buffer_context_root_options_t));
    buffer_ctx_opts.allocator = allocator;
    buffer_ctx_opts.buffer_capacity = POMELO_BUFFER_CAPACITY;
    buffer_ctx = pomelo_buffer_context_root_create(&buffer_ctx_opts);
    pomelo_check(buffer_ctx != NULL);

    // Create protocol context
    memset(&protocol_ctx_opts, 0, sizeof(pomelo_protocol_context_options_t));
    protocol_ctx_opts.allocator = allocator;
    protocol_ctx_opts.buffer_context = buffer_ctx;
    protocol_ctx_opts.payload_capacity = POMELO_BUFFER_CAPACITY;
    protocol_ctx = pomelo_protocol_context_create(&protocol_ctx_opts);
    pomelo_check(protocol_ctx != NULL);

    // Random values
    pomelo_random_buffer((uint8_t *) &protocol_id, sizeof(protocol_id));
    pomelo_random_buffer((uint8_t *) &client_id, sizeof(client_id));
    pomelo_random_buffer((uint8_t *) &sample_v1, sizeof(sample_v1));
    pomelo_random_buffer((uint8_t *) &sample_v2, sizeof(sample_v2));

    // Generate keys
    pomelo_random_buffer(
        crypto_ctx.private_key,
        sizeof(crypto_ctx.private_key)
    );

    pomelo_random_buffer(
        crypto_ctx.challenge_key,
        sizeof(crypto_ctx.challenge_key)
    );

    // Create connect token
    token.protocol_id = protocol_id;
    token.create_timestamp = pomelo_platform_now(platform);
    token.expire_timestamp = token.create_timestamp + TOKEN_EXPIRE * 1000ULL;
    pomelo_random_buffer(
        token.connect_token_nonce,
        sizeof(token.connect_token_nonce)
    );
    token.timeout = CONNECT_TIMEOUT;
    token.naddresses = 1;
    ret = pomelo_address_from_string(&token.addresses[0], SERVER_ADDRESS);
    pomelo_check(ret == 0);

    pomelo_random_buffer(
        token.client_to_server_key,
        sizeof(token.client_to_server_key)
    );
    pomelo_random_buffer(
        token.server_to_client_key,
        sizeof(token.server_to_client_key)
    );

    token.client_id = client_id;
    pomelo_random_buffer(token.user_data, sizeof(token.user_data));

    // Encode connect token
    ret = pomelo_connect_token_encode(
        connect_token,
        &token,
        crypto_ctx.private_key
    );
    pomelo_check(ret == 0);

    // Update codec context
    pomelo_reference_init(&crypto_ctx.ref, NULL);
    memcpy(
        crypto_ctx.packet_encrypt_key,
        token.client_to_server_key,
        sizeof(token.client_to_server_key)
    );
    memcpy(
        crypto_ctx.packet_decrypt_key,
        token.server_to_client_key,
        sizeof(token.server_to_client_key)
    );
    crypto_ctx.protocol_id = protocol_id;

    // Create client address
    ret = pomelo_address_from_string(&address, CLIENT_ADDRESS);
    pomelo_check(ret == 0);

    // Create adapter
    pomelo_adapter_options_t adapter_options = {
        .allocator = allocator,
        .platform = platform
    };
    adapter_client = pomelo_adapter_create(&adapter_options);
    pomelo_check(adapter_client != NULL);

    // Create server
    memset(&server_options, 0, sizeof(pomelo_protocol_server_options_t));
    server_options.context = protocol_ctx;
    server_options.max_clients = MAX_CLIENTS;
    server_options.platform = platform;
    server_options.sequencer = &sequencer;
    server_options.private_key = crypto_ctx.private_key;
    server_options.protocol_id = protocol_id;
    server_options.adapter = adapter_client;
    ret = pomelo_address_from_string(&server_options.address, SERVER_ADDRESS);
    pomelo_check(ret == 0);

    server = pomelo_protocol_server_create(&server_options);
    pomelo_check(server != NULL);

    // Set handler for client
    server->adapter->send_handler = send_handler;

    ret = pomelo_protocol_socket_start(server);
    pomelo_check(ret == 0);

    // After starting server, send request from client
    send_request_packet(NULL);

    // Run the loop
    uv_run(&uv_loop, UV_RUN_DEFAULT);
    uv_loop_close(&uv_loop);

    pomelo_protocol_socket_destroy(server);
    pomelo_adapter_destroy(adapter_client);

    // Check resource leak
    pomelo_statistic_protocol_t protocol_statistic;
    pomelo_protocol_context_statistic(protocol_ctx, &protocol_statistic);
    pomelo_statistic_protocol_check_resource_leak(&protocol_statistic);

    pomelo_statistic_buffer_t buffer_statistic;
    pomelo_buffer_context_statistic(buffer_ctx, &buffer_statistic);
    pomelo_statistic_buffer_check_resource_leak(&buffer_statistic);

    // Destroy platform and contexts
    pomelo_platform_uv_destroy(platform);
    pomelo_protocol_context_destroy(protocol_ctx);
    pomelo_buffer_context_destroy(buffer_ctx);

    // Check memleak
    pomelo_check(server_completed == 1);
    pomelo_check(alloc_bytes == pomelo_allocator_allocated_bytes(allocator));
    printf("Test passed!\n");
    return 0;
}
