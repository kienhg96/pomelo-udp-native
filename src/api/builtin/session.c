#include <assert.h>
#include <string.h>
#include "pomelo/errno.h"
#include "session.h"
#include "channel.h"
#include "api/socket.h"
#include "api/context.h"


/// @brief The initial size of channels array
#define POMELO_BUILTIN_SESSION_CHANNELS_INITIAL_SIZE 128


/* -------------------------------------------------------------------------- */
/*                                Public APIs                                 */
/* -------------------------------------------------------------------------- */

pomelo_session_methods_t * pomelo_session_builtin_methods(void) {
    static bool initialized = false;
    static pomelo_session_methods_t methods;
    if (initialized) {
        return &methods;
    }

    // Update methods
    methods.disconnect = (pomelo_session_disconnect_fn)
        pomelo_session_builtin_disconnect;
    methods.get_rtt = (pomelo_session_get_rtt_fn)
        pomelo_session_builtin_get_rtt;
    methods.get_channel = (pomelo_session_get_channel_fn)
        pomelo_session_builtin_get_channel;

    initialized = true;
    return &methods;
}


int pomelo_session_builtin_disconnect(pomelo_session_builtin_t * session) {
    assert(session != NULL);
    pomelo_protocol_peer_t * peer = session->peer;
    if (!peer) {
        return POMELO_ERR_SESSION_INVALID;
    }

    return pomelo_protocol_peer_disconnect(peer);
}


int pomelo_session_builtin_get_rtt(
    pomelo_session_builtin_t * session,
    uint64_t * mean,
    uint64_t * variance
) {
    assert(session != NULL);
    pomelo_delivery_endpoint_t * endpoint = session->endpoint;
    if (!endpoint) return POMELO_ERR_SESSION_INVALID;

    pomelo_delivery_endpoint_rtt(endpoint, mean, variance);
    return 0;
}


pomelo_channel_builtin_t * pomelo_session_builtin_get_channel(
    pomelo_session_builtin_t * session,
    size_t channel_index
) {
    assert(session != NULL);
    if (!session->channels) return NULL; // Invalid session

    pomelo_channel_builtin_t * channel = NULL;
    pomelo_array_get(session->channels, channel_index, &channel);
    return channel;
}


/* -------------------------------------------------------------------------- */
/*                                Private APIs                                */
/* -------------------------------------------------------------------------- */


int pomelo_session_builtin_on_alloc(
    pomelo_session_builtin_t * session,
    pomelo_context_t * context
) {
    assert(session != NULL);
    assert(context != NULL);

    int ret = pomelo_session_on_alloc(&session->base, context);
    if (ret < 0) return ret;

    pomelo_array_options_t array_options = {
        .allocator = context->allocator,
        .element_size = sizeof(pomelo_channel_builtin_t *),
        .initial_capacity = POMELO_BUILTIN_SESSION_CHANNELS_INITIAL_SIZE
    };
    session->channels = pomelo_array_create(&array_options);
    if (!session->channels) return -1;

    return 0;
}


void pomelo_session_builtin_on_free(pomelo_session_builtin_t * session) {
    assert(session != NULL);

    if (session->channels) {
        pomelo_array_destroy(session->channels);
        session->channels = NULL;
    }

    pomelo_session_on_free(&session->base);
}


int pomelo_session_builtin_init(
    pomelo_session_builtin_t * session,
    pomelo_session_builtin_info_t * info
) {
    assert(session != NULL);
    assert(info != NULL);
    pomelo_socket_t * socket = info->socket;
    pomelo_protocol_peer_t * peer = info->peer;

    // Initialize base session
    pomelo_session_info_t base_info = {
        .type = POMELO_SESSION_TYPE_BUILTIN,
        .socket = socket,
        .methods = pomelo_session_builtin_methods()
    };
    pomelo_session_t * base = &session->base;
    int ret = pomelo_session_init(base, &base_info);
    if (ret < 0) return ret; // Initialize base session failed

    // Acquire new delivery endpoint.
    pomelo_delivery_endpoint_options_t options = {
        .context = socket->context->delivery_context,
        .platform = socket->platform,
        .sequencer = &socket->sequencer,
        .heartbeat = socket->heartbeat,
        .nbuses = socket->channel_modes->size,
        .time_sync = (socket->state == POMELO_SOCKET_STATE_RUNNING_CLIENT)
    };
    pomelo_delivery_endpoint_t * endpoint =
        pomelo_delivery_endpoint_create(&options);
    if (!endpoint) return -1; // Failed to acquire new endpoint

    // Wrap endpoint and peer
    pomelo_delivery_endpoint_set_extra(endpoint, session);
    pomelo_protocol_peer_set_extra(peer, session);

    session->endpoint = endpoint;
    session->peer = peer;
    session->ready = false;
    base->client_id = pomelo_protocol_peer_get_client_id(peer);
    base->address = *pomelo_protocol_peer_get_address(peer);

    // Initialize the on disconnected task
    pomelo_sequencer_task_init(
        &session->on_disconnected_task,
        (pomelo_sequencer_callback)
            pomelo_session_builtin_on_disconnected_deferred,
        session
    );

    // Initialize channels. Address of channels array is at the end of channel.
    pomelo_array_t * channels = session->channels;
    pomelo_array_t * channel_modes = socket->channel_modes;
    size_t nchannels = channel_modes->size;
    pomelo_channel_mode mode = POMELO_CHANNEL_MODE_UNRELIABLE;

    // Resize the channels array
    ret = pomelo_array_resize(channels, nchannels);
    if (ret < 0) return ret; // Failed to resize channels array
    pomelo_array_fill_zero(channels); // Fill the array with zeros

    // Initialize all channels
    pomelo_pool_t * channel_pool = socket->context->builtin_channel_pool;
    for (size_t i = 0; i < nchannels; i++) {
        pomelo_delivery_bus_t * bus =
            pomelo_delivery_endpoint_get_bus(endpoint, i);
        assert(bus != NULL);
        pomelo_array_get(channel_modes, i, &mode);

        pomelo_channel_builtin_info_t info = {
            .session = session,
            .mode = mode,
            .bus = bus,
        };
        pomelo_channel_builtin_t * channel =
            pomelo_pool_acquire(channel_pool, &info);
        if (!channel) return -1; // Failed to acquire channel

        // Set the channel to array
        pomelo_array_set(channels, i, channel);
    }

    // Start the endpoint
    ret = pomelo_delivery_endpoint_start(endpoint);
    if (ret < 0) return ret; // Failed to start the endpoint

    return 0;
}


void pomelo_session_builtin_cleanup(pomelo_session_builtin_t * session) {
    assert(session != NULL);
    pomelo_session_t * base = &session->base;
    pomelo_context_t * context = base->context;
    pomelo_pool_t * channel_pool = context->builtin_channel_pool;

    // Release all channels
    pomelo_array_t * channels = session->channels;
    size_t nchannels = channels->size;
    for (size_t i = 0; i < nchannels; ++i) {
        pomelo_channel_builtin_t * channel = NULL;
        pomelo_array_get(channels, i, &channel);
        if (channel) {
            pomelo_pool_release(channel_pool, channel);
        }
    }
    pomelo_array_clear(channels);

    // Destroying the endpoint here
    if (session->endpoint) {
        pomelo_delivery_endpoint_stop(session->endpoint);
        pomelo_delivery_endpoint_destroy(session->endpoint);
        session->endpoint = NULL;
    }

    if (session->peer) {
        pomelo_protocol_peer_set_extra(session->peer, NULL);
        session->peer = NULL;
    }

    // Finally, cleanup base session
    pomelo_session_cleanup(base);
}


void pomelo_session_builtin_on_disconnected(
    pomelo_session_builtin_t * session
) {
    assert(session != NULL);
    pomelo_session_t * session_base = &session->base;
    pomelo_socket_t * socket = session_base->socket;

    if (session->peer) {
        // Peer may be unset before
        pomelo_protocol_peer_set_extra(session->peer, NULL);
        session->peer = NULL;
    }

    session->base.state = POMELO_SESSION_STATE_DISCONNECTED;
    pomelo_sequencer_submit(&socket->sequencer, &session->on_disconnected_task);
    // => pomelo_session_builtin_on_disconnected_deferred()
}


void pomelo_session_builtin_on_disconnected_deferred(
    pomelo_session_builtin_t * session
) {
    assert(session != NULL);

    // Stop the endpoint
    pomelo_delivery_endpoint_stop(session->endpoint);

    // Dispatch the disconnected event
    if (session->ready) {
        // Only dispatch the disconnected event if the session is ready
        pomelo_socket_on_disconnected(session->base.socket, &session->base);
    }

    // Release the endpoint & session
    pomelo_delivery_endpoint_set_extra(session->endpoint, NULL);
    pomelo_delivery_endpoint_destroy(session->endpoint);
    session->endpoint = NULL;

    // Remove session from list
    pomelo_socket_t * socket = session->base.socket;
    pomelo_socket_remove_session(socket, &session->base);
    pomelo_pool_release(socket->context->builtin_session_pool, session);
}
