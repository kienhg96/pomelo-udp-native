// Automated smoke test for the plugin-load path.
//
// It exercises the shared-library plugin lifecycle that is otherwise untested:
//   pomelo_plugin_load_by_path(argv[1])  -> load a plugin .so by path
//   pomelo_plugin_register(...)          -> register it with the context
//   pomelo_socket_listen(...)            -> start a plugin-aware server socket
//   (short timer) pomelo_socket_stop(...) -> drain the loop and shut down cleanly
//
// CTest passes the demo plugin artifact as argv[1]
// ($<TARGET_FILE:pomelo-test-demo-plugin>). The demo plugin only calls
// plugin->set_data in its entry point and configures no callbacks, so this is a
// shallow but real check that loading, registering and running a plugin-backed
// socket leaks nothing and terminates. It deliberately does NOT drive a real
// WebRTC transport; it just proves the plugin-load plumbing works end to end.
#include <inttypes.h>
#include <stdio.h>
#include "pomelo-test.h"
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

// File-scope so the stop timer callback can reach the running server socket.
static pomelo_socket_t * server = NULL;
static uv_timer_t stop_timer;


// Deterministic teardown: once the server is listening there is no client to
// drive the exchange, so a short timer stops the socket. Stopping drains the
// loop (the protocol server closes its handles asynchronously) and uv_run
// returns, which is what makes this an automated, self-terminating test rather
// than the forever-running manual harness it used to be.
static void on_stop_timer(uv_timer_t * handle) {
    uv_timer_stop(handle);
    uv_close((uv_handle_t *) handle, NULL);
    pomelo_socket_stop(server);
}


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
    pomelo_check(context != NULL);

    // Create platform
    uv_loop_t * uv_loop = pomelo_allocator_malloc_t(allocator, uv_loop_t);
    pomelo_check(uv_loop != NULL);
    uv_loop_init(uv_loop);

    pomelo_platform_uv_options_t platform_options;
    platform_options.allocator = allocator;
    platform_options.uv_loop = uv_loop;
    pomelo_platform_t * platform = pomelo_platform_uv_create(&platform_options);
    pomelo_check(platform != NULL);

    // Load plugin from the shared library path given on the command line.
    pomelo_plugin_initializer initializer = pomelo_plugin_load_by_path(argv[1]);
    pomelo_check(initializer != NULL);

    // Register plugin
    pomelo_plugin_t * plugin = pomelo_plugin_register(
        allocator,
        context,
        platform,
        initializer
    );
    pomelo_check(plugin != NULL);

    // Create new socket
    pomelo_socket_options_t socket_options = {
        .context = context,
        .platform = platform,
        .nchannels = NUMBER_OF_CHANNELS,
        .channel_modes = channel_modes
    };
    server = pomelo_socket_create(&socket_options);
    pomelo_check(server != NULL);

    // Start socket as server
    pomelo_address_t address;
    pomelo_check(pomelo_address_from_string(&address, address_str) == 0);

    int result = pomelo_socket_listen(
        server,
        private_key,
        protocol_id,
        max_clients,
        &address
    );
    pomelo_check(result == 0);
    pomelo_check(
        pomelo_socket_get_state(server) == POMELO_SOCKET_STATE_RUNNING_SERVER
    );

    printf("Server is listening on %s\n", address_str);

    // Arm the stop timer, then run the loop until it drains.
    uv_timer_init(uv_loop, &stop_timer);
    uv_timer_start(&stop_timer, on_stop_timer, 50, 0);

    // Run UV loop
    uv_run(uv_loop, UV_RUN_DEFAULT);

    // The socket must have reached STOPPED: if stop never completed, the loop
    // would not have drained and we would have hung instead of reaching here.
    pomelo_check(
        pomelo_socket_get_state(server) == POMELO_SOCKET_STATE_STOPPED
    );

    uv_loop_close(uv_loop);

    // Cleanup & check memleak
    pomelo_socket_destroy(server);
    pomelo_context_destroy(context);
    pomelo_platform_uv_destroy(platform);
    pomelo_allocator_free(allocator, uv_loop);

    pomelo_check(allocated_bytes == pomelo_allocator_allocated_bytes(allocator));
    printf("WebRTC plugin smoke test passed\n");
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
    pomelo_check(session != NULL);
    int64_t client_id = pomelo_session_get_client_id(session);
    printf("Session connected: %" PRIi64 "\n", client_id);
}


void pomelo_socket_on_disconnected(
    pomelo_socket_t * socket,
    pomelo_session_t * session
) {
    (void) socket;
    pomelo_check(session != NULL);
    int64_t client_id = pomelo_session_get_client_id(session);
    printf("Session disconnected: %" PRIi64 "\n", client_id);
}


void pomelo_socket_on_received(
    pomelo_socket_t * socket,
    pomelo_session_t * session,
    pomelo_message_t * message
) {
    (void) socket;
    pomelo_check(session != NULL);
    pomelo_check(message != NULL);

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
