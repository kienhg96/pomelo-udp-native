#include <string.h>
#include "uv.h"
#include "pomelo-test.h"
#include "pomelo/api.h"
#include "pomelo/platform.h"
#include "pomelo/random.h"
#include "pomelo/token.h"
#include "platform-test/platform-test.h"
#include "platform/platform.h" // watchdog timer

// Regression test for N4 (REVIEW.md section 6): destroying the context while
// protocol senders still have in-flight crypto WORKER tasks on the libuv
// threadpool must not abort with
//   Assertion failed: (packet != NULL), ... pomelo_protocol_packet_header_init
// at src/protocol/packet.c:629.
//
// Root cause (confirmed by trace, NOT packet-pool exhaustion):
//   On an encrypted socket every outgoing packet is encoded on a worker
//   threadpool thread (pomelo_protocol_sender_process -> submit_worker_task).
//   The abrupt teardown path (pomelo_context_destroy with no graceful socket
//   stop) runs pomelo_socket_cleanup -> pomelo_protocol_socket_destroy, which
//   only releases the client/server to its pool and NEVER reaches
//   client_stop/server_stop, so pomelo_protocol_peer_cancel_senders_and_receivers
//   is never called. The protocol context then destroys sender_pool via
//   pomelo_pool_root_destroy, which runs sender_cleanup (NULLs sender->packet)
//   and frees the sender backing UNCONDITIONALLY. Meanwhile a threadpool thread
//   is still inside sender_process_entry reading sender->packet -> NULL -> abort
//   (use-after-free race; nothing synchronizes the loop thread's free against
//   the threadpool thread's entry).
//
// This mirrors the pomelo-udp-node repro (8 worker_threads bursting messages,
// then worker.terminate()). To make the race reliably observable from one C
// process we connect a loopback client+server (encrypted, so handshake + every
// send is a worker task), burst many UNRELIABLE messages (each becomes a PAYLOAD
// sender + worker encrypt task; unreliable has no single-in-flight cap), then
// tear the context down immediately while those tasks are still on the pool.

#define TEST_PROTOCOL_ID 50
#define TEST_CHANNELS 4
#define TEST_MAX_CLIENTS 8
#define TEST_ADDRESS "127.0.0.1:8911"
#define TEST_TOKEN_EXPIRE (3600 * 1000)
#define TEST_TOKEN_TIMEOUT -1
#define TEST_CLIENT_ID 125
#define CH_UNRELIABLE 0
#define PAYLOAD_LEN 4000
#define BURST 4000

static pomelo_allocator_t * allocator;
static pomelo_context_t * context;
static pomelo_platform_t * platform;
static pomelo_socket_t * server;
static pomelo_socket_t * client;
static pomelo_session_t * client_session;
static pomelo_session_t * server_session;

static uint8_t private_key[POMELO_KEY_BYTES];
static uint8_t connect_token[POMELO_CONNECT_TOKEN_BYTES];
static pomelo_connect_token_t token;

static bool connected_client = false;
static bool connected_server = false;
static bool burst_done = false;

#define WATCHDOG_TIMEOUT_MS 5000
static volatile int timed_out = 0;
static pomelo_platform_handle_t watchdog_timer;

static const uint8_t PAYLOAD[PAYLOAD_LEN] =
    { 0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF };

static int init_connect_token(void) {
    pomelo_random_buffer(private_key, sizeof(private_key));
    token.protocol_id = TEST_PROTOCOL_ID;
    token.create_timestamp = pomelo_platform_now(platform);
    token.expire_timestamp = token.create_timestamp + TEST_TOKEN_EXPIRE;
    pomelo_random_buffer(
        token.connect_token_nonce, sizeof(token.connect_token_nonce));
    token.timeout = TEST_TOKEN_TIMEOUT;
    token.naddresses = 1;
    pomelo_address_from_string(&token.addresses[0], TEST_ADDRESS);
    pomelo_random_buffer(
        token.client_to_server_key, sizeof(token.client_to_server_key));
    pomelo_random_buffer(
        token.server_to_client_key, sizeof(token.server_to_client_key));
    token.client_id = TEST_CLIENT_ID;
    return pomelo_connect_token_encode(connect_token, &token, private_key);
}

static void watchdog_timeout(void * data) {
    (void) data;
    timed_out = 1;
    uv_stop(uv_default_loop());
}

// Both peers connected: burst many UNRELIABLE sends so a crowd of crypto worker
// tasks is in flight, then stop the loop before they drain.
static void on_ready(void) {
    if (burst_done) return;
    burst_done = true;

    pomelo_channel_t * channel =
        pomelo_session_get_channel(client_session, CH_UNRELIABLE);
    pomelo_check(channel != NULL);
    pomelo_check(pomelo_channel_set_mode(
        channel, POMELO_CHANNEL_MODE_UNRELIABLE) == 0);

    for (int i = 0; i < BURST; i++) {
        pomelo_message_t * message = pomelo_context_acquire_message(context);
        pomelo_check(message != NULL);
        pomelo_check(pomelo_message_write_buffer(
            message, PAYLOAD, PAYLOAD_LEN) == 0);
        pomelo_session_send(client_session, CH_UNRELIABLE, message, NULL);
        pomelo_message_unref(message); // drop caller ref
    }

    // Return to main with a crowd of worker encrypt tasks still in flight.
    uv_stop(uv_default_loop());
}

int main(void) {
    printf("API destroy-with-inflight-worker test\n");
    allocator = pomelo_allocator_default();
    uint64_t alloc_bytes = pomelo_allocator_allocated_bytes(allocator);

    platform = pomelo_test_platform_create(allocator);
    pomelo_check(platform != NULL);
    pomelo_check(init_connect_token() == 0);

    pomelo_context_root_options_t context_options;
    memset(&context_options, 0, sizeof(context_options));
    context_options.allocator = allocator;
    context = pomelo_context_root_create(&context_options);
    pomelo_check(context != NULL);

    pomelo_socket_options_t socket_options;
    memset(&socket_options, 0, sizeof(socket_options));
    socket_options.nchannels = TEST_CHANNELS;
    socket_options.platform = platform;
    socket_options.context = context;
    server = pomelo_socket_create(&socket_options);
    pomelo_check(server != NULL);

    pomelo_address_t address;
    pomelo_address_from_string(&address, TEST_ADDRESS);
    pomelo_check(pomelo_socket_listen(
        server, private_key, TEST_PROTOCOL_ID, TEST_MAX_CLIENTS, &address) == 0);

    memset(&socket_options, 0, sizeof(socket_options));
    socket_options.nchannels = TEST_CHANNELS;
    socket_options.platform = platform;
    socket_options.context = context;
    client = pomelo_socket_create(&socket_options);
    pomelo_check(client != NULL);
    pomelo_check(pomelo_socket_connect(client, connect_token) == 0);

    pomelo_check(pomelo_platform_timer_start(
        platform, watchdog_timeout, WATCHDOG_TIMEOUT_MS, 0, NULL,
        &watchdog_timer) == 0);

    // Run until on_ready() bursts and stops the loop with workers in flight.
    pomelo_test_platform_run(platform);
    pomelo_platform_timer_stop(platform, &watchdog_timer);
    pomelo_check(timed_out == 0);
    pomelo_check(burst_done == true);

    // Abrupt teardown: destroy the context WITHOUT a graceful socket stop while
    // crypto worker tasks are still in flight. Must not abort.
    pomelo_context_destroy(context);

    pomelo_test_platform_destroy(platform);

    pomelo_check(alloc_bytes == pomelo_allocator_allocated_bytes(allocator));
    return 0;
}


/* Required global callbacks */

void pomelo_socket_on_connected(
    pomelo_socket_t * socket, pomelo_session_t * session) {
    if (socket == client) {
        client_session = session;
        connected_client = true;
    } else {
        server_session = session;
        connected_server = true;
    }
    if (connected_client && connected_server) on_ready();
}

void pomelo_socket_on_disconnected(
    pomelo_socket_t * socket, pomelo_session_t * session) {
    (void) socket; (void) session;
}

void pomelo_socket_on_received(
    pomelo_socket_t * socket,
    pomelo_session_t * session,
    pomelo_message_t * message) {
    (void) socket; (void) session; (void) message;
}

void pomelo_socket_on_stopped(pomelo_socket_t * socket) {
    (void) socket;
}

void pomelo_socket_on_connect_result(
    pomelo_socket_t * socket, pomelo_socket_connect_result result) {
    (void) socket;
    pomelo_check(result == POMELO_SOCKET_CONNECT_SUCCESS);
}

void pomelo_socket_on_send_result(
    pomelo_socket_t * socket,
    pomelo_message_t * message,
    void * data,
    size_t send_count) {
    (void) socket; (void) message; (void) data; (void) send_count;
}

void pomelo_session_on_cleanup(pomelo_session_t * session) {
    (void) session;
}

void pomelo_channel_on_cleanup(pomelo_channel_t * channel) {
    (void) channel;
}
