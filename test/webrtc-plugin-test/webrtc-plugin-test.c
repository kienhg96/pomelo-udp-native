#include <assert.h>
#include <stdio.h>
#include <inttypes.h>
#include "pomelo/api.h"
#include "pomelo/constants.h"
#include "pomelo/platforms/platform-uv.h"


#define NUMBER_OF_CHANNELS 3
static pomelo_channel_mode channel_modes[] = {
    POMELO_CHANNEL_MODE_UNRELIABLE,
    POMELO_CHANNEL_MODE_SEQUENCED,
    POMELO_CHANNEL_MODE_RELIABLE
};

static uint8_t private_key[POMELO_KEY_BYTES] = { 1, 2, 3, 4 };
static uint64_t protocol_id = 5456;
static size_t max_clients = 100;
static const char * address_str = "127.0.0.1:8888";
static pomelo_context_t * context = NULL;


int main(int argc, char * argv[]) {
    if (argc < 2) {
        printf("Not enough argument. Usage webrtc-plugin-test <plugin_path>\n");
        return -1;
    }

    // Use default allocator
    pomelo_allocator_t * allocator = pomelo_allocator_default();
    size_t allocated_bytes = pomelo_allocator_allocated_bytes(allocator);

    // Create context
    pomelo_context_root_options_t context_options = {
        .allocator = allocator
    };
    context = pomelo_context_root_create(&context_options);
    if (!context) {
        return -1;
    }

    // Create platform
    uv_loop_t * uv_loop = pomelo_allocator_malloc_t(allocator, uv_loop_t);
    if (!uv_loop) {
        return -1;
    }
    uv_loop_init(uv_loop);

    pomelo_platform_uv_options_t platform_options;
    platform_options.allocator = allocator;
    platform_options.uv_loop = uv_loop;
    pomelo_platform_t * platform = pomelo_platform_uv_create(&platform_options);
    if (!platform) {
        return -1;
    }

    // Load plugin
    pomelo_plugin_initializer initializer = pomelo_plugin_load_by_path(argv[1]);
    if (!initializer) {
        printf("Failed to load plugin: %s\n", argv[1]);
        return -1;
    }

    // Register plugin
    pomelo_plugin_t * plugin = pomelo_plugin_register(
        allocator,
        context,
        platform,
        initializer
    );
    if (!plugin) {
        printf("Failed to register plugin: %s\n", argv[1]);
        return -1;
    }

    // Create new socket
    pomelo_socket_options_t socket_options = {
        .context = context,
        .platform = platform,
        .nchannels = NUMBER_OF_CHANNELS,
        .channel_modes = channel_modes
    };
    pomelo_socket_t * socket = pomelo_socket_create(&socket_options);
    if (!socket) {
        printf("Failed to create socket\n");
        return -1;
    }

    // Start socket as server
    pomelo_address_t address;
    if (pomelo_address_from_string(&address, address_str) < 0) {
        printf("Failed to parse address from string\n");
        return -1;
    }
    int result = pomelo_socket_listen(
        socket,
        private_key,
        protocol_id,
        max_clients,
        &address
    );
    if (result < 0) {
        printf("Failed to start socket as server\n");
        return -1;
    }

    printf("Server is listening on %s\n", address_str);

    // Run UV loop
    uv_run(uv_loop, UV_RUN_DEFAULT);

    // Cleanup & check memleak
    pomelo_socket_destroy(socket);
    pomelo_context_destroy(context);
    pomelo_platform_uv_destroy(platform);
    pomelo_allocator_free(allocator, uv_loop);

    if (allocated_bytes != pomelo_allocator_allocated_bytes(allocator)) {
        printf("Memleak detected.");
        return -1;
    }
    return 0;
}


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
    (void) socket;
    assert(session != NULL);
    int64_t client_id = pomelo_session_get_client_id(session);
    printf("Session connected: %" PRIi64 "\n", client_id);
}


void pomelo_socket_on_disconnected(
    pomelo_socket_t * socket,
    pomelo_session_t * session
) {
    (void) socket;
    assert(session != NULL);
    int64_t client_id = pomelo_session_get_client_id(session);
    printf("Session disconnected: %" PRIi64 "\n", client_id);
}


void pomelo_socket_on_received(
    pomelo_socket_t * socket,
    pomelo_session_t * session,
    pomelo_message_t * message
) {
    (void) socket;
    assert(session != NULL);
    assert(message != NULL);

    size_t size = pomelo_message_size(message);
    int64_t client_id = pomelo_session_get_client_id(session);
    printf("Session received: %" PRIi64 ": %zu bytes\n", client_id, size);
    int32_t value = 0;
    int ret = pomelo_message_read_int32(message, &value);
    if (ret == 0) {
        printf("Session received value: %d\n", value);
    }

    pomelo_message_t * reply = pomelo_context_acquire_message(context);
    if (!reply) return;

    pomelo_message_write_int32(reply, 78692);
    pomelo_session_send(session, 0, reply, NULL);
    pomelo_message_unref(reply);
}


void pomelo_socket_on_connect_result(
    pomelo_socket_t * socket,
    pomelo_socket_connect_result result
) {
    (void) socket;
    (void) result;
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
}
