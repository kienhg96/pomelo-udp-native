#include <string.h>
#include "pomelo-test.h"
#include "pomelo/api.h"
#include "pomelo/platform.h"
#include "pomelo/random.h"
#include "pomelo/token.h"
#include "platform-test/platform-test.h"
#include "statistic-check/statistic-check.h"
#include "soak-test/soak-common.h"

#define API_TEST_PROTOCOL_ID 50
#define API_TEST_CHANNELS 10
#define API_TEST_MAX_CLIENTS 32
#define API_TEST_ADDRESS "127.0.0.1:8888"
#define API_TEST_TOKEN_EXPIRE (3600 * 1000) // 1 hour
#define API_TEST_TOKEN_TIMEOUT -1
#define API_TEST_CLIENT_ID 125

#define API_PAYLOAD_BYTES 8
#define API_TARGET_INFLIGHT 6
#define API_MODE_COUNT 3

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

// Soak state
static pomelo_soak_t soak;
static pomelo_session_t * client_session = NULL;
static size_t api_in_flight = 0;
static size_t api_mode_index = 0;
static uint8_t api_payload[API_PAYLOAD_BYTES];
static const pomelo_channel_mode api_modes[API_MODE_COUNT] = {
    POMELO_CHANNEL_MODE_RELIABLE,
    POMELO_CHANNEL_MODE_SEQUENCED,
    POMELO_CHANNEL_MODE_UNRELIABLE
};

// Temp variables
static pomelo_context_root_options_t context_options;
static pomelo_socket_options_t socket_options;


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


static void api_send_one(void) {
    if (soak.stopped || !client_session) return;

    size_t ch = api_mode_index++ % API_MODE_COUNT;
    pomelo_channel_t * channel = pomelo_session_get_channel(client_session, ch);
    pomelo_check(channel != NULL);
    int ret = pomelo_channel_set_mode(channel, api_modes[ch]);
    pomelo_check(ret == 0);

    pomelo_message_t * message = pomelo_context_acquire_message(context);
    pomelo_check(message != NULL);
    ret = pomelo_message_write_buffer(message, api_payload, API_PAYLOAD_BYTES);
    pomelo_check(ret == 0);

    pomelo_session_send(client_session, ch, message, NULL);
    pomelo_message_unref(message);
    api_in_flight++;
}

static void api_pump(void) {
    while (!soak.stopped && client_session && api_in_flight < API_TARGET_INFLIGHT) {
        api_send_one();
    }
}

static void soak_sample(void * ud, uint64_t * values, size_t n) {
    (void) ud; (void) n;
    pomelo_statistic_t s;
    pomelo_context_statistic(context, &s);

    values[0] = s.api.messages;
    values[1] = s.api.builtin_sessions;
    values[2] = s.api.builtin_channels;
    values[3] = s.delivery.parcels;
    values[4] = s.delivery.senders;
    values[5] = s.delivery.receivers;
    values[6] = s.buffer.buffers;
    values[7] = s.allocator.allocated_bytes;
}

static void soak_stop(void * ud) {
    (void) ud;
    pomelo_socket_stop(server);
    pomelo_socket_stop(client);
}


int main(int argc, char ** argv) {
    printf("API soak\n");
    allocator = pomelo_allocator_default();
    uint64_t alloc_bytes = pomelo_allocator_allocated_bytes(allocator);

    platform = pomelo_test_platform_create(allocator);
    pomelo_check(platform != NULL);

    int ret = init_connect_token();
    pomelo_check(ret == 0);

    memset(&context_options, 0, sizeof(pomelo_context_root_options_t));
    context_options.allocator = allocator;
    context = pomelo_context_root_create(&context_options);
    pomelo_check(context != NULL);

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

    memset(&socket_options, 0, sizeof(pomelo_socket_options_t));
    socket_options.nchannels = API_TEST_CHANNELS;
    socket_options.platform = platform;
    socket_options.context = context;

    client = pomelo_socket_create(&socket_options);
    pomelo_check(client != NULL);

    ret = pomelo_socket_connect(client, connect_token);
    pomelo_check(ret == 0);

    memset(&soak, 0, sizeof(soak));
    soak.platform = platform;
    soak.sample = soak_sample;
    soak.stop = soak_stop;
    soak.progress_floor = 10;
    pomelo_random_buffer(api_payload, sizeof(api_payload));
    pomelo_soak_config_from_args(&soak, argc, argv);
    pomelo_soak_watch(&soak, "messages",     16, 0);
    pomelo_soak_watch(&soak, "sessions",      4, 0);
    pomelo_soak_watch(&soak, "channels",     16, 0);
    pomelo_soak_watch(&soak, "parcels",      16, 0);
    pomelo_soak_watch(&soak, "d_senders",    16, 0);
    pomelo_soak_watch(&soak, "d_receivers",  16, 0);
    pomelo_soak_watch(&soak, "buffers",      32, 0);
    pomelo_soak_watch(&soak, "alloc_bytes",   0, 25);
    pomelo_soak_start(&soak);

    pomelo_test_platform_run(platform);

    pomelo_soak_finish(&soak);

    pomelo_socket_destroy(server);
    pomelo_socket_destroy(client);

    pomelo_statistic_t statistic;
    pomelo_context_statistic(context, &statistic);
    pomelo_statistic_check_resource_leak(&statistic);

    pomelo_context_destroy(context);
    pomelo_test_platform_destroy(platform);

    pomelo_check(alloc_bytes == pomelo_allocator_allocated_bytes(allocator));
    printf("API soak done.\n");
    return 0;
}


/* Implementation of events */

void pomelo_session_on_cleanup(pomelo_session_t * session) {
    (void) session;
}


void pomelo_channel_on_cleanup(pomelo_channel_t * channel) {
    (void) channel;
}


void pomelo_socket_on_connected(
    pomelo_socket_t * socket,
    pomelo_session_t * session
) {
    pomelo_check(socket != NULL);
    pomelo_check(session != NULL);

    if (socket == client) {
        client_session = session;
        api_pump();
    }
}


void pomelo_socket_on_disconnected(
    pomelo_socket_t * socket,
    pomelo_session_t * session
) {
    pomelo_check(socket != NULL);
    pomelo_check(session != NULL);
    if (socket == client) {
        client_session = NULL;
    }
}


void pomelo_socket_on_received(
    pomelo_socket_t * socket,
    pomelo_session_t * session,
    pomelo_message_t * message
) {
    (void) session;
    (void) message;
    pomelo_check(socket != NULL);
    // The client send-result drives the pump; nothing to do here.
}


void pomelo_socket_on_stopped(pomelo_socket_t * socket) {
    (void) socket;
}


void pomelo_socket_on_connect_result(
    pomelo_socket_t * socket,
    pomelo_socket_connect_result result
) {
    (void) socket;
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

    if (api_in_flight) api_in_flight--;
    pomelo_soak_tick(&soak);
    if (!soak.stopped) api_pump();
}
