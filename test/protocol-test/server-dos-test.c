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
    Protocol server anti-DoS test (half-open / requesting peer cap):

    The server bounds the number of half-open (pending) handshakes. When a
    REQUEST arrives from an address with no existing peer, the server checks
    `requesting_peers->size >= max_clients` BEFORE allocating a requesting
    peer (src/protocol/server.c). The peer is allocated SYNCHRONOUSLY at
    REQUEST receipt (via the sequencer), before the async token decrypt.

    This test floods the server with FLOOD_COUNT REQUESTs from DISTINCT source
    addresses WITHOUT ever completing a handshake (no RESPONSE is sent and the
    server's outgoing packets are dropped), and asserts the number of active
    peers never grows beyond MAX_CLIENTS.
*/


// Constants
#define SERVER_ADDRESS "127.0.0.1:8888"
#define CLIENT_ADDRESS_IP "127.0.0.1"
#define CLIENT_BASE_PORT 10000
#define MAX_CLIENTS 4
#define FLOOD_COUNT 16
#define CONNECT_TIMEOUT 1 // seconds
#define TOKEN_EXPIRE 3600 // seconds
#define STOP_TIMER_MS 50  // let worker tasks drain, then stop the server


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
static pomelo_platform_handle_t stop_timer;

// Global variables
static pomelo_address_t address;
static uint64_t protocol_id;
static int64_t client_id;

// Peak number of active peers captured during the flood.
static volatile size_t peak_peers = 0;


/// @brief Encode, encrypt and dispatch a packet to the server from `src`
static void deliver_outgoing_packet(
    pomelo_protocol_packet_t * packet,
    pomelo_address_t * src
) {
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

    // Dispatch message to the server from the given source address
    pomelo_adapter_recv(server->adapter, src, &view);
    pomelo_buffer_unref(buffer);
}


/// @brief Send a single REQUEST packet from `src`
static void send_request_packet_from(pomelo_address_t * src) {
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
    deliver_outgoing_packet(&packet_request->base, src);

    // Release the packet
    pomelo_pool_release(
        protocol_ctx->packet_pools[POMELO_PROTOCOL_PACKET_REQUEST],
        packet_request
    );
}


/// @brief Flood the server with REQUESTs from distinct source addresses.
///        Peer allocation happens synchronously, so the active-peer count is
///        at its peak the moment this returns (before uv_run drains workers).
static void flood_requests(void) {
    printf("[i] Flooding server with %d REQUESTs from distinct addresses...\n",
        FLOOD_COUNT);

    for (int i = 0; i < FLOOD_COUNT; ++i) {
        pomelo_address_t src = address;
        // Vary the source port so each REQUEST maps to a new half-open peer.
        src.port = (uint16_t) (CLIENT_BASE_PORT + i);
        send_request_packet_from(&src);
    }

    // Capture the peak synchronously: every REQUEST has been processed (peers
    // allocated up to the cap), and no async worker completion has run yet.
    pomelo_statistic_protocol_t stat;
    pomelo_protocol_context_statistic(protocol_ctx, &stat);
    peak_peers = stat.peers;
    printf("[i] Peak active peers after flood: %zu (max_clients=%d)\n",
        peak_peers, MAX_CLIENTS);
}


/// @brief One-shot timer: stop the server so uv_run returns after draining.
static void stop_server_timer(void * data) {
    (void) data;
    printf("[i] Stopping server.\n");
    pomelo_protocol_socket_stop(server);
}


/* Required protocol callback symbols (stubs: no handshake completes here). */

void pomelo_protocol_socket_on_connected(
    pomelo_protocol_socket_t * socket,
    pomelo_protocol_peer_t * peer
) {
    (void) socket;
    (void) peer;
    // A half-open flood must never complete a handshake.
    pomelo_check(0 && "unexpected connection during DoS flood");
}


void pomelo_protocol_socket_on_disconnected(
    pomelo_protocol_socket_t * socket,
    pomelo_protocol_peer_t * peer
) {
    (void) socket;
    (void) peer;
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


void pomelo_protocol_socket_on_connect_result(
    pomelo_protocol_socket_t * socket,
    pomelo_protocol_connect_result result
) {
    (void) socket;
    (void) result;
}


int main(void) {
    printf("Test protocol server anti-DoS (half-open peer cap).\n");
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

    // Base client address (port is varied per REQUEST during the flood)
    ret = pomelo_address_from_string_ex(
        &address, CLIENT_ADDRESS_IP, CLIENT_BASE_PORT
    );
    pomelo_check(ret == 0);

    // Create adapter
    pomelo_adapter_options_t adapter_options = {
        .allocator = allocator,
        .platform = platform
    };
    adapter_client = pomelo_adapter_create(&adapter_options);
    pomelo_check(adapter_client != NULL);

    // Create server with a small max_clients so the cap is easy to hit
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

    // No send_handler: the server's outgoing CHALLENGE packets are dropped,
    // so flooded peers stay half-open and never progress the handshake.
    server->adapter->send_handler = NULL;

    ret = pomelo_protocol_socket_start(server);
    pomelo_check(ret == 0);

    // Flood the server and capture the peak active-peer count synchronously.
    flood_requests();

    // Schedule a one-shot timer to stop the server after workers drain.
    ret = pomelo_platform_timer_start(
        platform,
        stop_server_timer,
        STOP_TIMER_MS, // timeout
        0,             // no repeat (one-shot)
        NULL,
        &stop_timer
    );
    pomelo_check(ret == 0);

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

    // The cap must have held: never more than MAX_CLIENTS half-open peers,
    // even though FLOOD_COUNT (> MAX_CLIENTS) REQUESTs were delivered.
    pomelo_check(peak_peers <= MAX_CLIENTS);
    // Deterministic: all FLOOD_COUNT > MAX_CLIENTS requests were processed
    // synchronously before measurement, so the cap is saturated exactly.
    pomelo_check(peak_peers == MAX_CLIENTS);

    // Check memleak
    pomelo_check(alloc_bytes == pomelo_allocator_allocated_bytes(allocator));
    printf("Test passed!\n");
    return 0;
}
