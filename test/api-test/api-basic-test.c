#include <string.h>
#include "pomelo-test.h"
#include "pomelo/api.h"
#include "pomelo/platform.h"
#include "pomelo/random.h"
#include "pomelo/token.h"
#include "platform-test/platform-test.h"
#include "statistic-check/statistic-check.h"
#include "platform/platform.h" // Task 22: platform timer API for the watchdog

#define API_TEST_PROTOCOL_ID 50
#define API_TEST_CHANNELS 10
#define API_TEST_MAX_CLIENTS 32
#define API_TEST_ADDRESS "127.0.0.1:8888"
#define API_TEST_TOKEN_EXPIRE (3600 * 1000) // 1 hour
#define API_TEST_TOKEN_TIMEOUT -1 // 1 second
#define API_TEST_CLIENT_ID 125

// Task 15: per-mode channels. One channel per delivery mode so a single
// client->server exchange exercises RELIABLE, SEQUENCED and UNRELIABLE
// end-to-end with byte-verified payloads. Valid indices are 0..CHANNELS-1,
// so API_TEST_CHANNELS itself remains a convenient out-of-range index for the
// Task 13 invalid-channel regression below.
#define CH_RELIABLE   0
#define CH_SEQUENCED  1
#define CH_UNRELIABLE 2

#define PAYLOAD_LEN 5

// Distinct, byte-identifiable payloads, one per delivery mode. The first byte
// is a tag the receiver uses to route the message to the correct per-mode flag;
// the whole buffer is then memcmp'd to prove the bytes arrived intact. The
// in-process platform does not drop or reorder, so every payload is expected to
// arrive exactly once even for UNRELIABLE.
#define TAG_RELIABLE   0x01
#define TAG_SEQUENCED  0x02
#define TAG_UNRELIABLE 0x03

static const uint8_t RELIABLE_PAYLOAD[PAYLOAD_LEN] =
    { TAG_RELIABLE, 0xAA, 0xBB, 0xCC, 0xDD };
static const uint8_t SEQUENCED_PAYLOAD[PAYLOAD_LEN] =
    { TAG_SEQUENCED, 0x11, 0x22, 0x33, 0x44 };
static const uint8_t UNRELIABLE_PAYLOAD[PAYLOAD_LEN] =
    { TAG_UNRELIABLE, 0x55, 0x66, 0x77, 0x88 };


// Environment
static pomelo_allocator_t * allocator;
static pomelo_context_t * context;
static pomelo_platform_t * platform;

// Keys
static uint8_t private_key[POMELO_KEY_BYTES];
static uint8_t connect_token[POMELO_CONNECT_TOKEN_BYTES];
static pomelo_connect_token_t token;

// Sockets
static pomelo_socket_t * server;
static pomelo_socket_t * client;

// Sessions
static pomelo_session_t * server_session;
static pomelo_session_t * client_session;

// Global variables
static bool connected_client;
static bool connected_server;

// Task 15: per-mode receipt flags. Each must fire exactly once (the server
// verifies the payload bytes before incrementing). Completion waits on all
// three plus the matching valid send results so teardown never races ahead of
// the delivery/release of any in-flight message.
static volatile int got_reliable = 0;
static volatile int got_sequenced = 0;
static volatile int got_unreliable = 0;

// Number of valid (non-regression) send results observed. There are three
// client sends (one per mode), each to a single connected session.
static volatile int sent_valid_count = 0;

static volatile int exchange_done = 0;

// Task 22: watchdog. A one-shot platform timer that fires if the handshake or
// exchange stalls, turning a hang into a hard failure instead of a CTest
// timeout. Stopped on the normal completion path so it never fires on success
// and is fully torn down inside the run loop.
#define WATCHDOG_TIMEOUT_MS 5000
static volatile int timed_out = 0;
static pomelo_platform_handle_t watchdog_timer;

// Regression (Task 13): invalid-channel send result.
// A send on an out-of-range channel index must still dispatch a send result
// (as a failure) instead of silently dropping the message. The send-result
// callback below tags this specific send via a sentinel `data` pointer so it
// can be told apart from the normal (valid) sends.
static volatile int invalid_send_result_fired = 0;
static volatile size_t invalid_send_result_code = 1; // non-zero until captured
static int invalid_send_tag; // sentinel passed as `data` for the invalid send


// Temp variables
static pomelo_context_root_options_t context_options;
static pomelo_socket_options_t socket_options;


static int on_ready(void);
static void check_finished(void);
static void watchdog_timeout(void * data);


/// @brief Initialize connect token & keys
static int init_connect_token(void) {
    pomelo_random_buffer(private_key, sizeof(private_key));

    token.protocol_id = API_TEST_PROTOCOL_ID;
    token.create_timestamp = pomelo_platform_now(platform);
    token.expire_timestamp = token.create_timestamp + API_TEST_TOKEN_EXPIRE;
    pomelo_random_buffer(
        token.connect_token_nonce,
        sizeof(token.connect_token_nonce)
    );

    token.timeout = API_TEST_TOKEN_TIMEOUT;
    token.naddresses = 1;
    pomelo_address_from_string(&token.addresses[0], API_TEST_ADDRESS);

    pomelo_random_buffer(
        token.client_to_server_key,
        sizeof(token.client_to_server_key)
    );
    pomelo_random_buffer(
        token.server_to_client_key,
        sizeof(token.server_to_client_key)
    );
    token.client_id = API_TEST_CLIENT_ID;
    // Let the user_data empty

    int ret = pomelo_connect_token_encode(connect_token, &token, private_key);
    pomelo_check(ret == 0);

    return 0;
}


int main(void) {
    printf("API basic test\n");
    allocator = pomelo_allocator_default();
    uint64_t alloc_bytes = pomelo_allocator_allocated_bytes(allocator);

    // Create platform
    platform = pomelo_test_platform_create(allocator);
    pomelo_check(platform != NULL);

    // Create token
    int ret = init_connect_token();
    pomelo_check(ret == 0);

    // Create api context
    memset(&context_options, 0, sizeof(pomelo_context_root_options_t));
    context_options.allocator = allocator;
    context = pomelo_context_root_create(&context_options);
    pomelo_check(context != NULL);

    // Test messages
    pomelo_message_t * message = pomelo_context_acquire_message(context);

    pomelo_check(message != NULL);
    pomelo_check(pomelo_message_size(message) == 0);
    pomelo_check(pomelo_message_write_int32(message, 1234) == 0);
    pomelo_check(pomelo_message_size(message) == 4);

    pomelo_message_unref(message);

    // Create server
    memset(&socket_options, 0, sizeof(pomelo_socket_options_t));
    socket_options.nchannels = API_TEST_CHANNELS;
    socket_options.platform = platform;
    socket_options.context = context;

    server = pomelo_socket_create(&socket_options);
    pomelo_check(server != NULL);

    pomelo_address_t address;
    pomelo_address_from_string(&address, API_TEST_ADDRESS);

    ret = pomelo_socket_listen(
        server,
        private_key,
        API_TEST_PROTOCOL_ID,
        API_TEST_MAX_CLIENTS,
        &address
    );
    pomelo_check(ret == 0);

    // Create client
    memset(&socket_options, 0, sizeof(pomelo_socket_options_t));
    socket_options.nchannels = API_TEST_CHANNELS;
    socket_options.platform = platform;
    socket_options.context = context;

    client = pomelo_socket_create(&socket_options);
    pomelo_check(client != NULL);

    ret = pomelo_socket_connect(client, connect_token);
    pomelo_check(ret == 0);

    // Task 22: arm the watchdog before entering the run loop. A one-shot
    // (non-repeating) timer; repeat_ms == 0.
    ret = pomelo_platform_timer_start(
        platform,
        watchdog_timeout,
        WATCHDOG_TIMEOUT_MS,
        0, // one-shot
        NULL,
        &watchdog_timer
    );
    pomelo_check(ret == 0);

    pomelo_test_platform_run(platform);

    // Destroy the sockets
    pomelo_socket_destroy(server);
    pomelo_socket_destroy(client);

    // Get statistic to check resource leak
    pomelo_statistic_t statistic;
    pomelo_context_statistic(context, &statistic);

    pomelo_statistic_check_resource_leak(&statistic);

    pomelo_context_destroy(context);
    pomelo_test_platform_destroy(platform);

    // Task 15: every delivery mode must have delivered its payload intact,
    // exactly once, on its own channel. Asserted after uv_run has fully drained.
    pomelo_check(got_reliable == 1);
    pomelo_check(got_sequenced == 1);
    pomelo_check(got_unreliable == 1);
    pomelo_check(sent_valid_count == 3);

    // Regression (Task 13): the out-of-range channel send must have delivered
    // a send-result callback, reported as a failure (zero recipients).
    pomelo_check(invalid_send_result_fired == 1);
    pomelo_check(invalid_send_result_code == 0);

    // Check memleak
    pomelo_check(exchange_done == 1);
    // Task 22: the exchange must have completed within the watchdog window.
    pomelo_check(timed_out == 0);
    pomelo_check(alloc_bytes == pomelo_allocator_allocated_bytes(allocator));
    return 0;
}


/* Implementation of events */

void pomelo_client_on_connected(
    pomelo_socket_t * client,
    pomelo_session_t * session
) {
    (void) client;
    pomelo_track_function();
    client_session = session;

    connected_client = true;
    if (connected_server) {
        on_ready();
    }
}


void pomelo_client_on_disconnected(
    pomelo_socket_t * client,
    pomelo_session_t * session
) {
    (void) client;
    (void) session;

    pomelo_track_function();
}


void pomelo_client_on_received(
    pomelo_socket_t * client,
    pomelo_session_t * session,
    pomelo_message_t * message
) {
    (void) session;
    (void) client;
    (void) message;

    pomelo_track_function();

    // Task 15: the exchange is one-directional (client -> server), so the
    // client is not expected to receive any payload. Reaching here would mean a
    // mode echoed unexpectedly.
    pomelo_check(0 && "client received an unexpected message");
}


void pomelo_server_on_connected(
    pomelo_socket_t * server,
    pomelo_session_t * session
) {
    (void) server;

    pomelo_track_function();
    server_session = session;

    connected_server = true;
    if (connected_client) {
        on_ready();
    }
}


void pomelo_server_on_disconnected(
    pomelo_socket_t * server,
    pomelo_session_t * session
) {
    (void) server;
    (void) session;
    pomelo_track_function();
}


void pomelo_server_on_received(
    pomelo_socket_t * server,
    pomelo_session_t * session,
    pomelo_message_t * message
) {
    (void) server;
    (void) session;
    pomelo_track_function();

    // Task 15: byte-verify the payload and route to the matching per-mode flag.
    // The on-wire delivery mode is carried by the transmission itself, so the
    // receiver does not need to configure its channel mode to match the sender.
    uint8_t buffer[PAYLOAD_LEN];
    int ret = pomelo_message_read_buffer(message, buffer, PAYLOAD_LEN);
    pomelo_check(ret == 0);

    switch (buffer[0]) {
        case TAG_RELIABLE:
            pomelo_check(memcmp(buffer, RELIABLE_PAYLOAD, PAYLOAD_LEN) == 0);
            got_reliable++;
            printf("[i] {Server} RELIABLE payload OK.\n");
            break;

        case TAG_SEQUENCED:
            pomelo_check(memcmp(buffer, SEQUENCED_PAYLOAD, PAYLOAD_LEN) == 0);
            got_sequenced++;
            printf("[i] {Server} SEQUENCED payload OK.\n");
            break;

        case TAG_UNRELIABLE:
            pomelo_check(memcmp(buffer, UNRELIABLE_PAYLOAD, PAYLOAD_LEN) == 0);
            got_unreliable++;
            printf("[i] {Server} UNRELIABLE payload OK.\n");
            break;

        default:
            pomelo_check(0 && "unexpected payload tag");
            break;
    }
}


/// @brief Send one message on `channel_index` configured to `mode`, carrying
/// `payload`. Mirrors the acquire/write/send/unref idiom used elsewhere.
static void send_on_channel(
    size_t channel_index,
    pomelo_channel_mode mode,
    const uint8_t * payload
) {
    pomelo_message_t * message = pomelo_context_acquire_message(context);
    pomelo_check(message != NULL);

    int ret = pomelo_message_write_buffer(message, payload, PAYLOAD_LEN);
    pomelo_check(ret == 0);

    pomelo_channel_t * channel =
        pomelo_session_get_channel(client_session, channel_index);
    pomelo_check(channel != NULL);

    ret = pomelo_channel_set_mode(channel, mode);
    pomelo_check(ret == 0);

    pomelo_check(pomelo_channel_get_mode(channel) == mode);

    pomelo_session_send(client_session, channel_index, message, NULL);
    pomelo_message_unref(message);
}


/// Process when both client and server have connected
static int on_ready(void) {
    pomelo_track_function();

    // Task 15: one send per delivery mode, each on its own channel with a
    // distinct, byte-identifiable payload.
    send_on_channel(CH_RELIABLE, POMELO_CHANNEL_MODE_RELIABLE, RELIABLE_PAYLOAD);
    send_on_channel(
        CH_SEQUENCED, POMELO_CHANNEL_MODE_SEQUENCED, SEQUENCED_PAYLOAD
    );
    send_on_channel(
        CH_UNRELIABLE, POMELO_CHANNEL_MODE_UNRELIABLE, UNRELIABLE_PAYLOAD
    );

    // Regression (Task 13): send on an out-of-range channel index. Valid
    // indices are 0..API_TEST_CHANNELS-1, so API_TEST_CHANNELS is one past the
    // last valid index. The hardening fix in src/api/session.c must still
    // dispatch a send-result callback (reported as a failure) rather than
    // silently dropping the message. The sentinel `&invalid_send_tag` lets the
    // callback distinguish this send from the normal exchange.
    pomelo_message_t * invalid_message =
        pomelo_context_acquire_message(context);
    pomelo_check(invalid_message != NULL);

    uint8_t value = 99;
    int ret = pomelo_message_write_buffer(invalid_message, &value, 1);
    pomelo_check(ret == 0);

    pomelo_session_send(
        client_session,
        API_TEST_CHANNELS, // one past the last valid channel index
        invalid_message,
        &invalid_send_tag
    );
    pomelo_message_unref(invalid_message);

    return 0;
}


static void check_finished(void) {
    printf(
        "[i] got: reliable=%d sequenced=%d unreliable=%d, sent_valid=%d\n",
        got_reliable,
        got_sequenced,
        got_unreliable,
        sent_valid_count
    );

    // Wait until all three payloads have been received AND all three valid
    // sends have reported their result, so teardown never races in-flight
    // delivery or message release.
    if (!(got_reliable && got_sequenced && got_unreliable)) return;
    if (sent_valid_count < 3) return;

    exchange_done = 1;

    // Task 22: normal completion. Cancel the watchdog so it does not fire late
    // and so its handle is closed inside this run loop (the close callback
    // drains before pomelo_test_platform_run returns), avoiding a leak.
    pomelo_platform_timer_stop(platform, &watchdog_timer);

    printf("[i] Stopping client & server\n");
    pomelo_socket_stop(server);
    pomelo_socket_stop(client);
}


// Task 22: watchdog callback. The one-shot timer self-stops before this runs
// (see pomelo_platform_uv_timer_callback), so we only flag the failure and stop
// the sockets so the run loop drains and returns.
static void watchdog_timeout(void * data) {
    (void) data;
    timed_out = 1;
    printf("[!] Watchdog fired: exchange stalled, forcing teardown\n");
    pomelo_socket_stop(server);
    pomelo_socket_stop(client);
}


void pomelo_session_on_cleanup(pomelo_session_t * session) {
    (void) session;
    pomelo_track_function();
}


void pomelo_channel_on_cleanup(pomelo_channel_t * channel) {
    (void) channel;
    pomelo_track_function();
}


void pomelo_socket_on_connected(
    pomelo_socket_t * socket,
    pomelo_session_t * session
) {
    pomelo_check(socket != NULL);
    pomelo_check(session != NULL);

    if (socket == server) {
        pomelo_server_on_connected(socket, session);
    } else {
        pomelo_client_on_connected(socket, session);
    }
}


void pomelo_socket_on_disconnected(
    pomelo_socket_t * socket,
    pomelo_session_t * session
) {
    pomelo_check(socket != NULL);
    pomelo_check(session != NULL);

    if (socket == server) {
        pomelo_server_on_disconnected(socket, session);
    } else {
        pomelo_client_on_disconnected(socket, session);
    }
}


void pomelo_socket_on_received(
    pomelo_socket_t * socket,
    pomelo_session_t * session,
    pomelo_message_t * message
) {
    pomelo_check(socket != NULL);
    pomelo_check(session != NULL);
    pomelo_check(message != NULL);

    if (socket == server) {
        pomelo_server_on_received(socket, session, message);
    } else {
        pomelo_client_on_received(socket, session, message);
    }

    check_finished();
}


static void pomelo_socket_client_on_stopped(void) {
    pomelo_track_function();
}

static void pomelo_socket_server_on_stopped(void) {
    pomelo_track_function();
}

void pomelo_socket_on_stopped(pomelo_socket_t * socket) {
    if (socket == server) {
        pomelo_socket_server_on_stopped();
    } else {
        pomelo_socket_client_on_stopped();
    }
}


void pomelo_socket_on_connect_result(
    pomelo_socket_t * socket,
    pomelo_socket_connect_result result
) {
    (void) socket;
    (void) result;

    pomelo_track_function();
    pomelo_check(result == POMELO_SOCKET_CONNECT_SUCCESS);
}


void pomelo_socket_on_send_result(
    pomelo_socket_t * socket,
    pomelo_message_t * message,
    void * data,
    size_t send_count
) {
    (void) socket;
    (void) message;
    pomelo_track_function();

    // Regression (Task 13): the out-of-range channel send is tagged with the
    // sentinel pointer. Its result must still be dispatched here, reported as a
    // failure (send_count == 0, i.e. delivered to zero sessions).
    if (data == &invalid_send_tag) {
        invalid_send_result_fired = 1;
        invalid_send_result_code = send_count;
        return;
    }

    // Task 15: each valid per-mode send must report at least one recipient.
    pomelo_check(send_count > 0);

    sent_valid_count++;

    // Auto release flag is set, so we don't need to unref the message
    check_finished();
}
