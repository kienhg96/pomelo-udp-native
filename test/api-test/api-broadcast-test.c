#include <string.h>
#include "pomelo-test.h"
#include "pomelo/api.h"
#include "pomelo/platform.h"
#include "pomelo/token.h"
#include "pomelo/random.h"
#include "platform-test/platform-test.h"
#include "statistic-check/statistic-check.h"
#include "platform/platform.h" // Task 22: platform timer API for the watchdog

#define API_TEST_PROTOCOL_ID 50
#define API_TEST_CHANNELS 10
#define API_TEST_MAX_CLIENTS 32
#define API_TEST_ADDRESS "127.0.0.1:8888"
#define API_TEST_TOKEN_EXPIRE (3600 * 1000) // 1 hour
#define API_TEST_TOKEN_TIMEOUT -1 // 1 second
#define API_TEST_NCLIENTS 3


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
static pomelo_socket_t * clients[API_TEST_NCLIENTS];
static pomelo_session_t * sessions[API_TEST_NCLIENTS];


// Global variables
static size_t connected_counter = 0;
static size_t recv_counter = 0;
static size_t sent_counter = 0;

static volatile int client_received[API_TEST_NCLIENTS] = {0};
static volatile int distinct_received = 0;

// Task 22: watchdog. A one-shot platform timer that fires if the handshake or
// broadcast stalls, turning a hang into a hard failure instead of a CTest
// timeout. Stopped on the normal completion path so it never fires on success
// and is fully torn down inside the run loop.
#define WATCHDOG_TIMEOUT_MS 5000
static volatile int timed_out = 0;
static pomelo_platform_handle_t watchdog_timer;


// Temp variables
static pomelo_context_root_options_t context_options;
static pomelo_socket_options_t socket_options;


static int on_ready(void);
static void check_finished(void);
static void watchdog_timeout(void * data);


/// @brief Initialize connect token & keys
static int init_connect_token(int64_t client_id) {
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
    token.client_id = client_id;
    // Let the user_data empty

    int ret = pomelo_connect_token_encode(connect_token, &token, private_key);
    pomelo_check(ret == 0);

    return 0;
}


int main(void) {
    int ret = 0;

    printf("API broadcast test\n");
    allocator = pomelo_allocator_default();
    uint64_t alloc_bytes = pomelo_allocator_allocated_bytes(allocator);

    // Create platform
    platform = pomelo_test_platform_create(allocator);
    pomelo_check(platform != NULL);

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

    // Create private key
    pomelo_random_buffer(private_key, sizeof(private_key));

    ret = pomelo_socket_listen(
        server,
        private_key,
        API_TEST_PROTOCOL_ID,
        API_TEST_MAX_CLIENTS,
        &address
    );
    pomelo_check(ret == 0);

    // Create client
    for (int i = 0; i < API_TEST_NCLIENTS; i++) {
        // Create token
        ret = init_connect_token(i + 1000);
        pomelo_check(ret == 0);

        memset(&socket_options, 0, sizeof(pomelo_socket_options_t));
        socket_options.nchannels = API_TEST_CHANNELS;
        socket_options.platform = platform;
        socket_options.context = context;

        clients[i] = pomelo_socket_create(&socket_options);
        pomelo_check(clients[i] != NULL);

        ret = pomelo_socket_connect(clients[i], connect_token);
        pomelo_check(ret == 0);
    }

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
    for (int i = 0; i < API_TEST_NCLIENTS; i++) {
        pomelo_socket_destroy(clients[i]);
    }

    // Get statistic to check resource leak
    pomelo_statistic_t statistic;
    pomelo_context_statistic(context, &statistic);

    pomelo_statistic_check_resource_leak(&statistic);

    pomelo_context_destroy(context);
    pomelo_test_platform_destroy(platform);

    // Check memleak
    pomelo_check(distinct_received == API_TEST_NCLIENTS);
    pomelo_check(connected_counter == API_TEST_NCLIENTS);
    // Task 22: the broadcast must have completed within the watchdog window.
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
    (void) session;
    pomelo_track_function();
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
    pomelo_track_function();

    // Validate received message
    uint8_t value = 0;
    int ret = pomelo_message_read_buffer(message, &value, 1);
    pomelo_check(ret == 0);
    pomelo_check(value == 12);

    // Track distinct receiving clients (fan-out)
    for (int i = 0; i < API_TEST_NCLIENTS; i++) {
        if (client == clients[i]) {
            if (!client_received[i]) {
                client_received[i] = 1;
                distinct_received++;
            }
            break;
        }
    }
}


void pomelo_server_on_connected(
    pomelo_socket_t * server,
    pomelo_session_t * session
) {
    (void) server;
    pomelo_track_function();
    sessions[connected_counter++] = session;

    if (connected_counter == API_TEST_NCLIENTS) {
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
    (void) message;
    pomelo_track_function();
}


/// Process when both client and server have connected
static int on_ready(void) {
    pomelo_track_function();

    // Prepare a message to send from client to server
    pomelo_message_t * message = pomelo_context_acquire_message(context);
    pomelo_check(message != NULL);

    int ret;
    uint8_t value = 12;
    ret = pomelo_message_write_buffer(message, &value, 1);
    pomelo_check(ret == 0);

    for (int i = 0; i < API_TEST_NCLIENTS; i++) {
        pomelo_channel_t * channel =
            pomelo_session_get_channel(sessions[i], 0);
        pomelo_check(channel != NULL);

        ret = pomelo_channel_set_mode(channel, POMELO_CHANNEL_MODE_RELIABLE);
        pomelo_check(ret == 0);
    }

    // Dispatch the message to all clients
    pomelo_socket_send(
        server,
        0,
        message,
        sessions,
        sizeof(sessions) / sizeof(sessions[0]),
        NULL
    );
    pomelo_message_unref(message);

    return 0;
}


/// @brief Check if all clients have received the message
static void check_finished(void) {
    if (recv_counter < API_TEST_NCLIENTS ||
        sent_counter < API_TEST_NCLIENTS
    ) {
        return; // Not all clients have received the message
    }

    printf("[i] All clients have received the message\n");

    // Task 22: normal completion. Cancel the watchdog so it does not fire late
    // and so its handle is closed inside this run loop (the close callback
    // drains before pomelo_test_platform_run returns), avoiding a leak.
    pomelo_platform_timer_stop(platform, &watchdog_timer);

    // Stop the clients & server
    printf("[i] Stopping clients & server\n");
    for (int i = 0; i < API_TEST_NCLIENTS; i++) {
        pomelo_socket_stop(clients[i]);
    }
    pomelo_socket_stop(server);
}


// Task 22: watchdog callback. The one-shot timer self-stops before this runs,
// so we only flag the failure and stop the sockets so the run loop drains and
// returns.
static void watchdog_timeout(void * data) {
    (void) data;
    timed_out = 1;
    printf("[!] Watchdog fired: broadcast stalled, forcing teardown\n");
    for (int i = 0; i < API_TEST_NCLIENTS; i++) {
        pomelo_socket_stop(clients[i]);
    }
    pomelo_socket_stop(server);
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

    recv_counter++;
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
    (void) data;
    pomelo_track_function();
    printf("[i] On send result send_count: %zu\n", send_count);
    // Auto release flag is set, so we don't need to unref the message

    sent_counter += send_count;
    check_finished();
}
