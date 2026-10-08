#include <string.h>
#include "pomelo-test.h"
#include "pomelo/api.h"
#include "pomelo/platform.h"
#include "pomelo/random.h"
#include "pomelo/token.h"
#include "platform-test/platform-test.h"
#include "statistic-check/statistic-check.h"
#include "soak-test/soak-common.h"

#define ENGINE_PROTOCOL_ID 50
#define ENGINE_CHANNELS 10
#define ENGINE_MAX_CLIENTS 32
#define ENGINE_ADDRESS "127.0.0.1:8888"
#define ENGINE_TOKEN_EXPIRE (3600 * 1000) // 1 hour
#define ENGINE_TOKEN_TIMEOUT -1
#define ENGINE_BASE_CLIENT_ID 125

#define ENGINE_CLIENTS 4
#define ENGINE_PAYLOAD_BYTES 8
#define ENGINE_TARGET_INFLIGHT 4
#define ENGINE_MODE_COUNT 3

typedef struct engine_client_s {
    pomelo_socket_t * socket;
    pomelo_session_t * session;
    size_t in_flight;
    size_t mode_index;
} engine_client_t;

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
static engine_client_t clients[ENGINE_CLIENTS];

// Soak state
static pomelo_soak_t soak;
static uint8_t engine_payload[ENGINE_PAYLOAD_BYTES];
static const pomelo_channel_mode engine_modes[ENGINE_MODE_COUNT] = {
    POMELO_CHANNEL_MODE_RELIABLE,
    POMELO_CHANNEL_MODE_SEQUENCED,
    POMELO_CHANNEL_MODE_UNRELIABLE
};

// Temp variables
static pomelo_context_root_options_t context_options;
static pomelo_socket_options_t socket_options;


/// @brief Initialize keys & the base connect token (shared fields)
static void init_connect_token(void) {
    pomelo_random_buffer(private_key, sizeof(private_key));

    token.protocol_id = ENGINE_PROTOCOL_ID;
    token.timeout = ENGINE_TOKEN_TIMEOUT;
    token.naddresses = 1;
    pomelo_address_from_string(&token.addresses[0], ENGINE_ADDRESS);

    pomelo_random_buffer(
        token.client_to_server_key,
        sizeof(token.client_to_server_key)
    );
    pomelo_random_buffer(
        token.server_to_client_key,
        sizeof(token.server_to_client_key)
    );
    // Let the user_data empty
}


/// @brief Encode a connect token for the client with the given id
static void encode_connect_token(int64_t client_id) {
    token.client_id = client_id;
    token.create_timestamp = pomelo_platform_now(platform);
    token.expire_timestamp = token.create_timestamp + ENGINE_TOKEN_EXPIRE;
    pomelo_random_buffer(
        token.connect_token_nonce,
        sizeof(token.connect_token_nonce)
    );

    int ret = pomelo_connect_token_encode(connect_token, &token, private_key);
    pomelo_check(ret == 0);
}


static engine_client_t * engine_find_by_socket(pomelo_socket_t * socket) {
    for (size_t i = 0; i < ENGINE_CLIENTS; i++) {
        if (clients[i].socket == socket) return &clients[i];
    }
    return NULL;
}


static void engine_send(engine_client_t * c) {
    if (soak.stopped || !c->session) return;

    size_t ch = c->mode_index++ % ENGINE_MODE_COUNT;
    pomelo_channel_t * channel = pomelo_session_get_channel(c->session, ch);
    pomelo_check(channel != NULL);
    int ret = pomelo_channel_set_mode(channel, engine_modes[ch]);
    pomelo_check(ret == 0);

    pomelo_message_t * message = pomelo_context_acquire_message(context);
    pomelo_check(message != NULL);
    ret = pomelo_message_write_buffer(
        message, engine_payload, ENGINE_PAYLOAD_BYTES
    );
    pomelo_check(ret == 0);

    pomelo_session_send(c->session, ch, message, NULL);
    pomelo_message_unref(message);
    c->in_flight++;
}


static void engine_pump(engine_client_t * c) {
    while (
        !soak.stopped && c->session &&
        c->in_flight < ENGINE_TARGET_INFLIGHT
    ) {
        engine_send(c);
    }
}


static void soak_sample(void * ud, uint64_t * values, size_t n) {
    (void) ud; (void) n;
    pomelo_statistic_t s;
    pomelo_context_statistic(context, &s);

    values[0] = s.protocol.peers;
    values[1] = s.protocol.packets;
    values[2] = s.delivery.parcels;
    values[3] = s.delivery.senders;
    values[4] = s.delivery.receivers;
    values[5] = s.api.messages;
    values[6] = s.api.builtin_sessions;
    values[7] = s.api.builtin_channels;
    values[8] = s.buffer.buffers;
    values[9] = s.allocator.allocated_bytes;
}


static void soak_stop(void * ud) {
    (void) ud;
    for (size_t i = 0; i < ENGINE_CLIENTS; i++) {
        pomelo_socket_stop(clients[i].socket);
    }
    pomelo_socket_stop(server);
}


int main(int argc, char ** argv) {
    printf("Engine soak\n");
    allocator = pomelo_allocator_default();
    uint64_t alloc_bytes = pomelo_allocator_allocated_bytes(allocator);

    platform = pomelo_test_platform_create(allocator);
    pomelo_check(platform != NULL);

    init_connect_token();

    memset(&context_options, 0, sizeof(pomelo_context_root_options_t));
    context_options.allocator = allocator;
    context = pomelo_context_root_create(&context_options);
    pomelo_check(context != NULL);

    memset(&socket_options, 0, sizeof(pomelo_socket_options_t));
    socket_options.nchannels = ENGINE_CHANNELS;
    socket_options.platform = platform;
    socket_options.context = context;

    server = pomelo_socket_create(&socket_options);
    pomelo_check(server != NULL);

    pomelo_address_t address;
    pomelo_address_from_string(&address, ENGINE_ADDRESS);

    int ret = pomelo_socket_listen(
        server,
        private_key,
        ENGINE_PROTOCOL_ID,
        ENGINE_MAX_CLIENTS,
        &address
    );
    pomelo_check(ret == 0);

    memset(clients, 0, sizeof(clients));
    for (size_t i = 0; i < ENGINE_CLIENTS; i++) {
        memset(&socket_options, 0, sizeof(pomelo_socket_options_t));
        socket_options.nchannels = ENGINE_CHANNELS;
        socket_options.platform = platform;
        socket_options.context = context;

        clients[i].socket = pomelo_socket_create(&socket_options);
        pomelo_check(clients[i].socket != NULL);

        encode_connect_token(ENGINE_BASE_CLIENT_ID + (int64_t) i);
        ret = pomelo_socket_connect(clients[i].socket, connect_token);
        pomelo_check(ret == 0);
    }

    memset(&soak, 0, sizeof(soak));
    soak.platform = platform;
    soak.sample = soak_sample;
    soak.stop = soak_stop;
    soak.progress_floor = 10;
    pomelo_random_buffer(engine_payload, sizeof(engine_payload));
    pomelo_soak_config_from_args(&soak, argc, argv);
    pomelo_soak_watch(&soak, "peers",         8, 0);
    pomelo_soak_watch(&soak, "packets",      48, 0);
    pomelo_soak_watch(&soak, "parcels",      32, 0);
    pomelo_soak_watch(&soak, "d_senders",    32, 0);
    pomelo_soak_watch(&soak, "d_receivers",  32, 0);
    pomelo_soak_watch(&soak, "messages",     32, 0);
    pomelo_soak_watch(&soak, "sessions",      8, 0);
    pomelo_soak_watch(&soak, "channels",     32, 0);
    pomelo_soak_watch(&soak, "buffers",      64, 0);
    pomelo_soak_watch(&soak, "alloc_bytes",   0, 25);
    pomelo_soak_start(&soak);

    pomelo_test_platform_run(platform);

    pomelo_soak_finish(&soak);

    pomelo_socket_destroy(server);
    for (size_t i = 0; i < ENGINE_CLIENTS; i++) {
        pomelo_socket_destroy(clients[i].socket);
    }

    pomelo_statistic_t statistic;
    pomelo_context_statistic(context, &statistic);
    pomelo_statistic_check_resource_leak(&statistic);

    pomelo_context_destroy(context);
    pomelo_test_platform_destroy(platform);

    pomelo_check(alloc_bytes == pomelo_allocator_allocated_bytes(allocator));
    printf("Engine soak done.\n");
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

    engine_client_t * c = engine_find_by_socket(socket);
    if (c) {
        c->session = session;
        engine_pump(c);
    }
}


void pomelo_socket_on_disconnected(
    pomelo_socket_t * socket,
    pomelo_session_t * session
) {
    pomelo_check(socket != NULL);
    pomelo_check(session != NULL);

    engine_client_t * c = engine_find_by_socket(socket);
    if (c) {
        c->session = NULL;
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
    (void) message;
    (void) data;
    (void) send_count;

    engine_client_t * c = engine_find_by_socket(socket);
    if (c && c->in_flight) c->in_flight--;
    pomelo_soak_tick(&soak);
    if (c && !soak.stopped) engine_pump(c);
}
