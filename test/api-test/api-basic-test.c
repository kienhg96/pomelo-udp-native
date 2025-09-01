#include <string.h>
#include "pomelo-test.h"
#include "pomelo/api.h"
#include "pomelo/platform.h"
#include "pomelo/random.h"
#include "pomelo/token.h"
#include "platform-test/platform-test.h"
#include "statistic-check/statistic-check.h"

#define API_TEST_PROTOCOL_ID 50
#define API_TEST_CHANNELS 10
#define API_TEST_MAX_CLIENTS 32
#define API_TEST_ADDRESS "127.0.0.1:8888"
#define API_TEST_TOKEN_EXPIRE (3600 * 1000) // 1 hour
#define API_TEST_TOKEN_TIMEOUT -1 // 1 second
#define API_TEST_CLIENT_ID 125
#define API_TEST_CHANNEL 5


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

static size_t sent_counter = 0;
static size_t recv_counter = 0;


// Temp variables
static pomelo_context_root_options_t context_options;
static pomelo_socket_options_t socket_options;


static int on_ready(void);
static void check_finished(void);


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

    // Check memleak
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
    
    pomelo_track_function();

    // Check received message
    uint8_t value = 0;
    int ret = pomelo_message_read_buffer(message, &value, 1);
    pomelo_check(ret == 0);
    pomelo_check(value == 24);

    printf("[i] {Client} Received message is OK.\n");

    check_finished();
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

    // Check received message
    uint8_t value;
    int ret;

    ret = pomelo_message_read_buffer(message, &value, 1);
    pomelo_check(ret == 0);
    pomelo_check(value == 12);

    printf("[i] {Server} Received message is OK.\n");

    pomelo_message_t * reply = pomelo_context_acquire_message(context);
    pomelo_check(reply != NULL);

    value = 24;
    ret = pomelo_message_write_buffer(reply, &value, 1);
    pomelo_check(ret == 0);

    pomelo_channel_t * channel =
        pomelo_session_get_channel(server_session, API_TEST_CHANNEL);
    pomelo_check(channel != NULL);

    // Change the channel to reliable
    ret = pomelo_channel_set_mode(channel, POMELO_CHANNEL_MODE_RELIABLE);
    pomelo_check(ret == 0);

    pomelo_session_t * sessions[] = { server_session }; 
    pomelo_socket_send(
        server,
        API_TEST_CHANNEL,
        reply,
        sessions,
        sizeof(sessions) / sizeof(sessions[0]),
        NULL
    );
    pomelo_message_unref(reply);
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

    pomelo_channel_t * channel =
        pomelo_session_get_channel(client_session, API_TEST_CHANNEL);
    pomelo_check(channel != NULL);

    // Change the channel to reliable
    ret = pomelo_channel_set_mode(channel, POMELO_CHANNEL_MODE_RELIABLE);
    pomelo_check(ret == 0);

    pomelo_session_send(
        client_session,
        API_TEST_CHANNEL,
        message,
        NULL
    );
    pomelo_message_unref(message);
    return 0;
}


static void check_finished(void) {
    printf(
        "[i] sent_counter: %zu, recv_counter: %zu\n",
        sent_counter,
        recv_counter
    );
    if (sent_counter < 2 || recv_counter < 2) return;

    printf("[i] Stopping client & server\n");
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
    (void) send_count;
    pomelo_track_function();
    pomelo_check(send_count > 0);

    sent_counter += send_count;

    // Auto release flag is set, so we don't need to unref the message
    check_finished();
}
