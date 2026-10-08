#include <assert.h>
#include "pomelo-test.h"
#include "pomelo/random.h"
#include "protocol/protocol.h"
#include "adapter/adapter.h"
#include "pomelo/platforms/platform-uv.h"
#include "crypto/crypto.h"
#include "protocol/socket.h"
#include "statistic-check/statistic-check.h"
#include "soak-test/soak-common.h"

/*
    With unencrypted test: We have to write a customized adapter since
    default adapter does not support unencrypted packets.
    
    Test script:
        - Client connects to server
        - After connected, client send a payload to server
        - Then, server echos the same message to client
        - Finish test
*/

// Constants
#define SOCKET_ADDRESS "127.0.0.1:8888"
#define MAX_CLIENTS 10
#define CONNECT_TIMEOUT 10 // seconds
#define TOKEN_EXPIRE 3600 // seconds


// Libraries
static uv_loop_t uv_loop;
static pomelo_allocator_t * allocator;
static pomelo_platform_t * platform;
static pomelo_sequencer_t sequencer;

// Adapters
static pomelo_adapter_t * adapter_client;
static pomelo_adapter_t * adapter_server;

// Contexts
static pomelo_buffer_context_t * buffer_ctx;
static pomelo_protocol_context_t * protocol_ctx;

// Sockets
static pomelo_protocol_socket_t * client;
static pomelo_protocol_socket_t * server;

// Key & token
static uint8_t private_key[POMELO_KEY_BYTES];
static uint8_t connect_token[POMELO_CONNECT_TOKEN_BYTES];

// Global variables
static int connected_count;

// Soak state
#define PROTO_PAYLOAD_BYTES 256
#define PROTO_TARGET_INFLIGHT 4
static pomelo_soak_t soak;
static pomelo_protocol_peer_t * client_peer = NULL;
static size_t proto_in_flight = 0;
static uint8_t proto_payload[PROTO_PAYLOAD_BYTES];

static uint64_t protocol_id;
static int64_t client_id;
static int32_t sample_v1;
static uint64_t sample_v2;

// Temp variables
static pomelo_connect_token_t token;
static pomelo_platform_uv_options_t platform_options;
static pomelo_protocol_context_options_t protocol_ctx_opts;
static pomelo_buffer_context_root_options_t buffer_ctx_opts;
static pomelo_protocol_client_options_t client_options;
static pomelo_protocol_server_options_t server_options;


static void proto_send_one(void) {
    if (soak.stopped || !client_peer) return;
    pomelo_buffer_t * buffer = pomelo_buffer_context_acquire(buffer_ctx);
    pomelo_check(buffer != NULL);
    memcpy(buffer->data, proto_payload, sizeof(proto_payload));

    pomelo_buffer_view_t view;
    view.buffer = buffer;
    view.offset = 0;
    view.length = sizeof(proto_payload);

    int ret = pomelo_protocol_peer_send(client_peer, &view, 1);
    pomelo_check(ret == 0);
    pomelo_buffer_unref(buffer);
    proto_in_flight++;
}

static void proto_pump(void) {
    while (!soak.stopped && client_peer && proto_in_flight < PROTO_TARGET_INFLIGHT) {
        proto_send_one();
    }
}

static void soak_sample(void * ud, uint64_t * values, size_t n) {
    (void) ud; (void) n;
    pomelo_statistic_protocol_t p;
    pomelo_protocol_context_statistic(protocol_ctx, &p);
    pomelo_statistic_buffer_t b;
    pomelo_buffer_context_statistic(buffer_ctx, &b);

    values[0] = p.peers;
    values[1] = p.senders;
    values[2] = p.receivers;
    values[3] = p.packets;
    values[4] = p.crypto_contexts;
    values[5] = b.buffers;
}

static void soak_stop(void * ud) {
    (void) ud;
    pomelo_protocol_socket_stop(client);
    pomelo_protocol_socket_stop(server);
}

int main(int argc, char ** argv) {
    printf("Protocol soak\n");

    if (pomelo_crypto_init() < 0) {
        printf("Failed to initialize codec.\n");
        return -1;
    }

    int ret = 0;
    allocator = pomelo_allocator_default();
    size_t alloc_bytes = pomelo_allocator_allocated_bytes(allocator);

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
    protocol_ctx_opts.buffer_context = (pomelo_buffer_context_t *) buffer_ctx;
    protocol_ctx_opts.payload_capacity = POMELO_PACKET_BODY_CAPACITY;
    protocol_ctx = pomelo_protocol_context_create(&protocol_ctx_opts);
    pomelo_check(protocol_ctx != NULL);

    // Generate private key
    pomelo_random_buffer(private_key, sizeof(private_key));

    // Random values
    pomelo_random_buffer((uint8_t *) &protocol_id, sizeof(protocol_id));
    pomelo_random_buffer((uint8_t *) &client_id, sizeof(client_id));
    pomelo_random_buffer((uint8_t *) &sample_v1, sizeof(sample_v1));
    pomelo_random_buffer((uint8_t *) &sample_v2, sizeof(sample_v2));

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
    ret = pomelo_address_from_string(&token.addresses[0], SOCKET_ADDRESS);
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

    // Create adapter
    pomelo_adapter_options_t adapter_options = {
        .allocator = allocator,
        .platform = platform
    };
    adapter_client = pomelo_adapter_create(&adapter_options);
    pomelo_check(adapter_client != NULL);
    adapter_server = pomelo_adapter_create(&adapter_options);
    pomelo_check(adapter_server != NULL);

    // Encode connect token
    ret = pomelo_connect_token_encode(connect_token, &token, private_key);
    pomelo_check(ret == 0);

    // Create client
    memset(&client_options, 0, sizeof(pomelo_protocol_client_options_t));
    client_options.context = protocol_ctx;
    client_options.platform = platform;
    client_options.adapter = adapter_client;
    client_options.connect_token = connect_token;
    client_options.sequencer = &sequencer;
    client = pomelo_protocol_client_create(&client_options);
    pomelo_check(client != NULL);

    // Create server
    memset(&server_options, 0, sizeof(pomelo_protocol_server_options_t));
    server_options.context = protocol_ctx;
    server_options.platform = platform;
    server_options.adapter = adapter_server;
    server_options.sequencer = &sequencer;
    server_options.private_key = private_key;
    server_options.protocol_id = protocol_id;
    server_options.max_clients = MAX_CLIENTS;
    ret = pomelo_address_from_string(&server_options.address, SOCKET_ADDRESS);
    pomelo_check(ret == 0);
    server = pomelo_protocol_server_create(&server_options);
    pomelo_check(server != NULL);

    // Check the capacity of server
    uint32_t capability = pomelo_adapter_get_capability(adapter_server);
    printf("Adapter capability: %u\n", capability);
    if (capability & POMELO_ADAPTER_CAPABILITY_SERVER_ENCRYPTED) {
        printf(" + Server encrypted\n");
    }
    if (capability & POMELO_ADAPTER_CAPABILITY_SERVER_UNENCRYPTED) {
        printf(" + Server unencrypted\n");
    }
    if (capability & POMELO_ADAPTER_CAPABILITY_CLIENT_ENCRYPTED) {
        printf(" + Client encrypted\n");
    }
    if (capability & POMELO_ADAPTER_CAPABILITY_CLIENT_UNENCRYPTED) {
        printf(" + Client unencrypted\n");
    }

    // Start server
    ret = pomelo_protocol_socket_start(server);
    pomelo_check(ret == 0);

    // Start client
    ret = pomelo_protocol_socket_start(client);
    pomelo_check(ret == 0);

    memset(&soak, 0, sizeof(soak));
    soak.platform = platform;
    soak.sample = soak_sample;
    soak.stop = soak_stop;
    soak.progress_floor = 10;
    pomelo_random_buffer(proto_payload, sizeof(proto_payload));
    pomelo_soak_config_from_args(&soak, argc, argv);
    pomelo_soak_watch(&soak, "peers",           4, 0);
    pomelo_soak_watch(&soak, "senders",        16, 0);
    pomelo_soak_watch(&soak, "receivers",      16, 0);
    pomelo_soak_watch(&soak, "packets",        32, 0);
    pomelo_soak_watch(&soak, "crypto_contexts", 4, 0);
    pomelo_soak_watch(&soak, "buffers",        32, 0);
    pomelo_soak_start(&soak);

    // Run
    uv_run(&uv_loop, UV_RUN_DEFAULT);

    // Cleanup
    uv_loop_close(&uv_loop);

    pomelo_soak_finish(&soak);

    pomelo_protocol_socket_destroy(server);
    pomelo_protocol_socket_destroy(client);
    pomelo_adapter_destroy(adapter_client);
    pomelo_adapter_destroy(adapter_server);

    // Check resource leak
    pomelo_statistic_protocol_t protocol_statistic;
    pomelo_protocol_context_statistic(protocol_ctx, &protocol_statistic);
    pomelo_statistic_protocol_check_resource_leak(&protocol_statistic);

    pomelo_statistic_buffer_t buffer_statistic;
    pomelo_buffer_context_statistic(buffer_ctx, &buffer_statistic);
    pomelo_statistic_buffer_check_resource_leak(&buffer_statistic);

    pomelo_platform_uv_destroy(platform);
    pomelo_protocol_context_destroy(protocol_ctx);
    pomelo_buffer_context_destroy(buffer_ctx);

    // Check memleak
    pomelo_check(alloc_bytes == pomelo_allocator_allocated_bytes(allocator));

    printf("Protocol soak done.\n");
    return 0;
}

/* -------------------------------------------------------------------------- */
/*                         Socket callbacks implementation                    */
/* -------------------------------------------------------------------------- */

void pomelo_protocol_socket_on_connected(
    pomelo_protocol_socket_t * socket,
    pomelo_protocol_peer_t * peer
) {
    assert(socket != NULL);
    assert(peer != NULL);

    connected_count++;
    if (socket == client) {
        client_peer = peer;
        proto_pump();
    } else {
        printf("Server connected.\n");
    }
}


void pomelo_protocol_socket_on_disconnected(
    pomelo_protocol_socket_t * socket,
    pomelo_protocol_peer_t * peer
) {
    assert(socket != NULL);
    (void) peer;
    if (socket == client) {
        client_peer = NULL;
    }
}


void pomelo_protocol_socket_on_received(
    pomelo_protocol_socket_t * socket,
    pomelo_protocol_peer_t * peer,
    pomelo_buffer_view_t * view
) {
    assert(socket != NULL);
    assert(peer != NULL);
    assert(view != NULL);

    if (socket == server) {
        // Echo back; the soak timer drives teardown.
        pomelo_protocol_peer_send(peer, view, 1);
    } else {
        if (proto_in_flight) proto_in_flight--;
        pomelo_soak_tick(&soak);
        if (!soak.stopped) proto_pump();
    }
}


void pomelo_protocol_socket_on_connect_result(
    pomelo_protocol_socket_t * socket,
    pomelo_protocol_connect_result result
) {
    (void) socket;
    if (result != POMELO_PROTOCOL_SOCKET_CONNECT_SUCCESS) {
        printf("Failed to connect to server\n");
        pomelo_protocol_socket_stop(server);
    }
}
