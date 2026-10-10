#include <assert.h>
#include <string.h>
#include "pomelo/errno.h"
#include "socket.h"
#include "context.h"
#include "session.h"
#include "message.h"
#include "channel.h"
#include "protocol/socket.h"
#include "protocol/client.h"
#include "protocol/server.h"
#include "builtin/session.h"
#include "builtin/channel.h"
#include "plugin/plugin.h"
#include "plugin/socket.h"
#include "plugin/manager.h"
#include "plugin/session.h"


int pomelo_socket_on_alloc(
    pomelo_socket_t * socket,
    pomelo_context_t * context
) {
    assert(socket != NULL);
    assert(context != NULL);

    pomelo_extra_set(socket->extra, NULL);
    socket->context = context;

    // Create sessions list
    pomelo_list_options_t list_options = {
        .allocator = context->allocator,
        .element_size = sizeof(pomelo_session_t *)
    };
    socket->sessions = pomelo_list_create(&list_options);
    if (!socket->sessions) return -1;

    // Create channel modes array
    pomelo_array_options_t array_options = {
        .allocator = context->allocator,
        .element_size = sizeof(pomelo_channel_mode)
    };
    socket->channel_modes = pomelo_array_create(&array_options);
    if (!socket->channel_modes) return -1;

    return 0;
}


void pomelo_socket_on_free(pomelo_socket_t * socket) {
    assert(socket != NULL);
    
    if (socket->sessions) {
        pomelo_list_destroy(socket->sessions);
        socket->sessions = NULL;
    }

    if (socket->channel_modes) {
        pomelo_array_destroy(socket->channel_modes);
        socket->channel_modes = NULL;
    }
}


int pomelo_socket_init(
    pomelo_socket_t * socket,
    pomelo_socket_options_t * options
) {
    assert(socket != NULL);
    assert(options != NULL);
    if (options->nchannels == 0) return -1;

    socket->platform = options->platform;

    pomelo_context_t * context = options->context;
    pomelo_allocator_t * allocator = context->allocator;

    // Initialize channel template array
    pomelo_array_t * channel_modes = socket->channel_modes;
    pomelo_array_resize(channel_modes, options->nchannels);
    if (options->channel_modes) {
        for (size_t i = 0; i < options->nchannels; i++) {
            pomelo_array_set(channel_modes, i, options->channel_modes[i]);
        }
    } else {
        // Set all channels to unreliable
        pomelo_channel_mode mode = POMELO_CHANNEL_MODE_UNRELIABLE;
        for (size_t i = 0; i < options->nchannels; i++) {
            pomelo_array_set(channel_modes, i, mode);
        }
    }

    // The protocol socket will be created later
    socket->protocol_socket = NULL;

    // Initialize sequencer
    pomelo_sequencer_init(&socket->sequencer);

    // Create adapter
    pomelo_adapter_options_t adapter_options = {
        .allocator = allocator,
        .platform = options->platform
    };
    socket->adapter = pomelo_adapter_create(&adapter_options);
    if (!socket->adapter) return -1;

    // Create heartbeat
    pomelo_delivery_heartbeat_options_t heartbeat_options = {
        .context = context->delivery_context,
        .platform = options->platform
    };
    socket->heartbeat = pomelo_delivery_heartbeat_create(&heartbeat_options);
    if (!socket->heartbeat) return -1;

    // Initialize the destroy task
    pomelo_sequencer_task_init(
        &socket->destroy_task,
        (pomelo_sequencer_callback) pomelo_socket_destroy_deferred,
        socket
    );

    // Initialize the stop task
    pomelo_sequencer_task_init(
        &socket->stop_task,
        (pomelo_sequencer_callback) pomelo_socket_stop_deferred,
        socket
    );

    // Dispatch to plugins
    pomelo_plugin_dispatch_socket_on_created(socket);
    return 0;
}


void pomelo_socket_cleanup(pomelo_socket_t * socket) {
    assert(socket != NULL);
    pomelo_extra_set(socket->extra, NULL);
    socket->flags = 0;

    if (socket->protocol_socket) {
        // Abrupt teardown: the context is being destroyed without a graceful
        // socket stop (e.g. pomelo_context_destroy called while sends are still
        // in flight). Stop the protocol socket first so its in-flight senders
        // are cancelled and the adapter stops receiving, then drain the crypto
        // worker tasks so every sender is released through its own completion
        // callback BEFORE the protocol context frees the sender pool. Without
        // this, a threadpool thread could still be running sender_process_entry
        // on a sender whose backing memory the loop thread has already freed
        // (use-after-free; aborts at packet.c:629 / sender.c:218). On the
        // graceful path this block is skipped: pomelo_socket_stop_deferred has
        // already stopped, destroyed and NULLed protocol_socket.
        pomelo_protocol_socket_stop(socket->protocol_socket);
        pomelo_platform_drain_worker_tasks(socket->platform);
        pomelo_protocol_socket_destroy(socket->protocol_socket);
        socket->protocol_socket = NULL;
    }

    if (socket->channel_modes) {
        pomelo_array_resize(socket->channel_modes, 0);
    }

    if (socket->adapter) {
        pomelo_adapter_destroy(socket->adapter);
        socket->adapter = NULL;
    }

    if (socket->heartbeat) {
        pomelo_delivery_heartbeat_destroy(socket->heartbeat);
        socket->heartbeat = NULL;
    }
}


/* -------------------------------------------------------------------------- */
/*                               Public APIs                                  */
/* -------------------------------------------------------------------------- */


pomelo_socket_t * pomelo_socket_create(pomelo_socket_options_t * options) {
    assert(options != NULL);

    if (!options->context) return NULL;
    if (options->nchannels == 0 || options->nchannels > POMELO_MAX_CHANNELS) {
        return NULL; // Invalid number of channels
    }

    return pomelo_pool_acquire(options->context->socket_pool, options);
}


void pomelo_socket_destroy(pomelo_socket_t * socket) {
    assert(socket != NULL);
    pomelo_sequencer_submit(&socket->sequencer, &socket->destroy_task);
    // => pomelo_socket_destroy_deferred()
}


void pomelo_socket_set_extra(pomelo_socket_t * socket, void * data) {
    assert(socket != NULL);
    pomelo_extra_set(socket->extra, data);
}


void * pomelo_socket_get_extra(pomelo_socket_t * socket) {
    assert(socket != NULL);
    return pomelo_extra_get(socket->extra);
}


int pomelo_socket_connect(
    pomelo_socket_t * socket,
    const uint8_t * connect_token
) {
    assert(socket != NULL);
    assert(connect_token != NULL);

    if (socket->state != POMELO_SOCKET_STATE_STOPPED) {
        // It is not stopped
        return POMELO_ERR_SOCKET_ILLEGAL_STATE;
    }

    // Create protocol socket first
    pomelo_protocol_client_options_t client_options = {
        .connect_token = connect_token,
        .context = socket->context->protocol_context,
        .platform = socket->platform,
        .sequencer = &socket->sequencer,
        .adapter = socket->adapter
    };
    socket->protocol_socket = pomelo_protocol_client_create(&client_options);
    if (!socket->protocol_socket) {
        // Failed to create client
        return POMELO_ERR_SOCKET_CONNECT;
    }

    // Set the socket as the extra data of protocol socket
    pomelo_protocol_socket_set_extra(socket->protocol_socket, socket);

    // Finally, start the socket
    int ret = pomelo_protocol_socket_start(socket->protocol_socket);

    if (ret == 0) {
        // Change the state to running
        socket->state = POMELO_SOCKET_STATE_RUNNING_CLIENT;
        socket->stopped_components = 0; // Reset the stopped components flag
    }

    // Dispatch to plugins
    pomelo_plugin_dispatch_socket_on_connecting(socket, connect_token);
    return ret;
}


int pomelo_socket_listen(
    pomelo_socket_t * socket,
    const uint8_t * private_key,
    uint64_t protocol_id,
    size_t max_clients,
    pomelo_address_t * address
) {
    assert(socket != NULL);
    assert(private_key != NULL);
    assert(address != NULL);

    if (socket->state != POMELO_SOCKET_STATE_STOPPED) {
        // It is still running
        return POMELO_ERR_SOCKET_ILLEGAL_STATE;
    }

    if (max_clients == 0) {
        // Invalid argument
        return POMELO_ERR_SOCKET_INVALID_ARG;
    }

    pomelo_protocol_server_options_t server_options = {
        .context = socket->context->protocol_context,
        .platform = socket->platform,
        .sequencer = &socket->sequencer,
        .max_clients = max_clients,
        .private_key = private_key,
        .protocol_id = protocol_id,
        .address = *address,
        .adapter = socket->adapter
    };
    socket->protocol_socket = pomelo_protocol_server_create(&server_options);
    if (!socket->protocol_socket) {
        return POMELO_ERR_SOCKET_LISTEN;
    }

    // Set the socket as the extra data of protocol socket
    pomelo_protocol_socket_set_extra(socket->protocol_socket, socket);

    // Start the socket
    int ret = pomelo_protocol_socket_start(socket->protocol_socket);
    if (ret < 0) return ret;

    // Change the state
    socket->state = POMELO_SOCKET_STATE_RUNNING_SERVER;
    socket->stopped_components = 0; // Reset the stopped components flag
    memcpy(socket->private_key, private_key, POMELO_KEY_BYTES);
    
    // Dispatch to plugins
    pomelo_plugin_dispatch_socket_on_listening(
        socket,
        &((pomelo_protocol_server_t *) socket->protocol_socket)->address
    );

    return 0;
}


void pomelo_socket_stop(pomelo_socket_t * socket) {
    assert(socket != NULL);

    if (socket->state != POMELO_SOCKET_STATE_RUNNING_CLIENT &&
        socket->state != POMELO_SOCKET_STATE_RUNNING_SERVER
    ) return; // The socket is not running now

    // Set stopping state
    socket->state = POMELO_SOCKET_STATE_STOPPING;
    pomelo_sequencer_submit(&socket->sequencer, &socket->stop_task);
    // => pomelo_socket_stop_deferred()
}


pomelo_socket_state pomelo_socket_get_state(pomelo_socket_t * socket) {
    assert(socket != NULL);
    return socket->state;
}


size_t pomelo_socket_get_nsessions(pomelo_socket_t * socket) {
    assert(socket != NULL);
    return socket->sessions->size;
}


void pomelo_socket_send(
    pomelo_socket_t * socket,
    size_t channel_index,
    pomelo_message_t * message,
    pomelo_session_t ** sessions,
    size_t nsessions,
    void * data
) {
    assert(socket != NULL);
    assert(message != NULL);
    assert(sessions != NULL);

    // Prepare the message for sending
    pomelo_message_prepare_send(message, data);

    if (channel_index >= socket->channel_modes->size) {
        pomelo_socket_dispatch_send_result(socket, message);
        return; // Invalid channel index
    }

    if (nsessions == 0) {
        pomelo_socket_dispatch_send_result(socket, message);
        return; // No sessions to send
    }

    /**
     * Process the sessions array:
     * 1. Set the original index of sessions
     * 2. Filter out the sessions that are not connected
     * 3. Sort the sessions array: All builtin sessions are in the front
     *    and all plugin sessions are in the back.
     * 4. Dispatch to plugins
     * 5. Dispatch to transporter
     */

    // 1. Set the original index of sessions
    for (size_t i = 0; i < nsessions; i++) {
        sessions[i]->tmp_original_index = i;
    }

    // 2. Filter out the sessions that are not connected
    size_t left = 0;
    size_t right = nsessions - 1;
    while (left < right) {
        // Find the first disconnected session from left
        while (
            left < right &&
            sessions[left]->state == POMELO_SESSION_STATE_CONNECTED
        ) left++;

        // Find the first connected session from right
        while (
            left < right &&
            sessions[right]->state != POMELO_SESSION_STATE_CONNECTED
        ) right--;

        // Swap the sessions
        if (left < right) {
            pomelo_session_t * tmp = sessions[left];
            sessions[left] = sessions[right];
            sessions[right] = tmp;
        }   
    }
    size_t nconnected = 
        (left + (sessions[left]->state == POMELO_SESSION_STATE_CONNECTED));

    if (nconnected == 0) {
        pomelo_socket_dispatch_send_result(socket, message);
        return; // No connected sessions
    }

    // 3. Sort the sessions array: All builtin sessions are in the front
    // and all plugin sessions are in the back.

    // Sort the sessions array
    left = 0;
    right = nconnected - 1;
    while (left < right) {
        // Find the first plugin session from left
        while (
            left < right &&
            sessions[left]->type == POMELO_SESSION_TYPE_BUILTIN
        ) left++;

        // Find the first builtin session from right
        while (
            left < right &&
            sessions[right]->type == POMELO_SESSION_TYPE_PLUGIN
        ) right--;

        // Swap the sessions
        if (left < right) {
            pomelo_session_t * tmp = sessions[left];
            sessions[left] = sessions[right];
            sessions[right] = tmp;
        }
    }
    
    // Count the number of builtin and plugin sessions
    size_t nbuiltin =
        (left + (sessions[left]->type == POMELO_SESSION_TYPE_BUILTIN));
    size_t nplugin = nsessions - nbuiltin;

    // Dispatch message to plugins
    if (nplugin > 0) {
        message->nsent += pomelo_socket_send_plugin(
            socket,
            message,
            channel_index,
            sessions + nbuiltin,
            nplugin
        );
    }

    // Dispatch message to builtin sessions
    if (nbuiltin > 0) {
        int ret = pomelo_socket_send_builtin(
            socket,
            message,
            channel_index,
            sessions,
            nbuiltin
        );
        if (ret < 0) {
            // Failed to send message to builtin sessions
            pomelo_socket_dispatch_send_result(socket, message);
            return;
        }
    }

    // Restore the original placement of sessions
    size_t i = 0;
    while (i < nsessions) {
        pomelo_session_t * session = sessions[i];
        if (session->tmp_original_index == i) {
            // The session is already in the correct position
            i++;
            continue;
        }

        // Swap the session to the correct position
        pomelo_session_t * tmp = sessions[i];
        sessions[i] = sessions[session->tmp_original_index];
        sessions[session->tmp_original_index] = tmp;
    }

    if (nbuiltin == 0) {
        // No builtin sessions to send, dispatch the result directly
        pomelo_socket_dispatch_send_result(socket, message);
    }
}


uint64_t pomelo_socket_time(pomelo_socket_t * socket) {
    assert(socket != NULL);
    // If this socket is a server, return hrtime
    // If this socket is a client, add the current hrtime with the offset of
    // the first builtin session

    if (socket->state == POMELO_SOCKET_STATE_RUNNING_SERVER) {
        // If socket is running as a server, return hrtime directly
        return pomelo_platform_hrtime(socket->platform);
    }

    if (socket->state == POMELO_SOCKET_STATE_RUNNING_CLIENT) {
        // Find the first builtin session
        pomelo_session_t * session = NULL;
        pomelo_list_iterator_t it;
        pomelo_list_iterator_init(&it, socket->sessions);
        while (pomelo_list_iterator_next(&it, &session) == 0) {
            if (session->type == POMELO_SESSION_TYPE_BUILTIN) {
                break;
            }
        }

        if (!session) return 0; // No builtin session

        pomelo_delivery_endpoint_t * endpoint =
            ((pomelo_session_builtin_t *) session)->endpoint;
        if (!endpoint) return 0; // No peer

        // Get the offset of the builtin session
        uint64_t base = pomelo_platform_hrtime(socket->platform);
        int64_t offset = pomelo_delivery_endpoint_time_offset(endpoint);
        return (uint64_t) (base + offset);
    }

    return 0;
}


pomelo_context_t * pomelo_socket_get_context(pomelo_socket_t * socket) {
    assert(socket != NULL);
    return socket->context;
}


pomelo_platform_t * pomelo_socket_get_platform(pomelo_socket_t * socket) {
    assert(socket != NULL);
    return socket->platform;
}


size_t pomelo_socket_get_nchannels(pomelo_socket_t * socket) {
    assert(socket != NULL);
    return socket->channel_modes->size;
}


pomelo_channel_mode pomelo_socket_get_channel_mode(
    pomelo_socket_t * socket,
    size_t channel_index
) {
    assert(socket != NULL);
    if (channel_index >= socket->channel_modes->size) {
        return POMELO_CHANNEL_MODE_UNRELIABLE;
    }
    pomelo_channel_mode mode;
    pomelo_array_get(socket->channel_modes, channel_index, &mode);
    return mode;
}


pomelo_adapter_t * pomelo_socket_get_adapter(pomelo_socket_t * socket) {
    assert(socket != NULL);
    return socket->adapter;
}


/* -------------------------------------------------------------------------- */
/*                               Private APIs                                 */
/* -------------------------------------------------------------------------- */


void pomelo_socket_add_session(
    pomelo_socket_t * socket,
    pomelo_session_t * session
) {
    assert(socket != NULL);
    assert(session != NULL);
    session->entry = pomelo_list_push_back(socket->sessions, session);
}


void pomelo_socket_remove_session(
    pomelo_socket_t * socket,
    pomelo_session_t * session
) {
    assert(socket != NULL);
    assert(session != NULL);

    if (session->entry) {
        pomelo_list_remove(socket->sessions, session->entry);
        session->entry = NULL;
    }
}


void pomelo_socket_dispatch_send_result(
    pomelo_socket_t * socket,
    pomelo_message_t * message
) {
    assert(socket != NULL);
    assert(message != NULL);

    // Keep reference of message
    pomelo_message_ref(message);

    // Finish sending, this will unset the busy flag of message
    pomelo_message_finish_send(message);

    // Call the callback
    pomelo_socket_on_send_result(
        socket,
        message,
        message->send_callback_data,
        message->nsent
    );

    // Release the message
    pomelo_message_unref(message);
}


int pomelo_session_iterator_init(
    pomelo_session_iterator_t * iterator,
    pomelo_socket_t * socket
) {
    assert(socket != NULL);
    assert(iterator != NULL);
    pomelo_list_t * sessions = socket->sessions;

    iterator->signature = sessions->mod_count;
    iterator->state = sessions->front;

    return 0;
}


int pomelo_socket_send_builtin(
    pomelo_socket_t * socket,
    pomelo_message_t * message,
    size_t channel_index,
    pomelo_session_t ** sessions,
    size_t nsessions
) {
    assert(socket != NULL);
    assert(message != NULL);
    assert(sessions != NULL);

    // Create new sender
    pomelo_delivery_sender_options_t options = {
        .context = socket->context->delivery_context,
        .parcel = message->parcel,
        .platform = socket->platform
    };
    pomelo_delivery_sender_t * sender = pomelo_delivery_sender_create(&options);
    if (!sender) return -1; // Failed to create sender
    pomelo_delivery_sender_set_extra(sender, socket);

    for (size_t i = 0; i < nsessions; i++) {
        pomelo_session_t * session = sessions[i];
        assert(session->type == POMELO_SESSION_TYPE_BUILTIN);

        pomelo_channel_t * channel =
            pomelo_session_get_channel(session, channel_index);
        assert(channel != NULL);

        pomelo_channel_mode mode = channel->methods->get_mode(channel);
        int ret = pomelo_delivery_sender_add_transmission(
            sender,
            ((pomelo_channel_builtin_t *) channel)->bus,
            (pomelo_delivery_mode) mode
        );
        if (ret < 0) {
            pomelo_delivery_sender_cancel(sender);
            return ret;
        }
    }

    pomelo_delivery_sender_submit(sender);
    return 0;
}


size_t pomelo_socket_send_plugin(
    pomelo_socket_t * socket,
    pomelo_message_t * message,
    size_t channel_index,
    pomelo_session_t ** sessions,
    size_t nsessions
) {
    assert(socket != NULL);
    assert(message != NULL);
    assert(sessions != NULL);

    size_t nsent = 0;
    for (size_t i = 0; i < nsessions; i++) {
        pomelo_session_t * session = sessions[i];
        assert(session->type == POMELO_SESSION_TYPE_PLUGIN);

        pomelo_plugin_impl_t * plugin =
            ((pomelo_session_plugin_t *) session)->plugin;
        if (plugin->session_on_send_callback) {
            // We can pack the message multiple times
            pomelo_message_pack(message);
            plugin->session_on_send_callback(
                &plugin->base,
                session,
                channel_index,
                message
            );
            pomelo_plugin_post_callback_cleanup(plugin);
            nsent++;
        }
    }

    pomelo_message_unpack(message);
    return nsent;
}


int pomelo_session_iterator_next(
    pomelo_session_iterator_t * iterator,
    pomelo_session_t ** p_session
) {
    assert(iterator != NULL);
    assert(p_session != NULL);

    pomelo_list_entry_t * entry = iterator->state;
    if (!entry) {
        *p_session = NULL;
        return 0; // No more element
    }

    pomelo_session_t * session = pomelo_list_element(entry, pomelo_session_t *);
    pomelo_socket_t * socket = session->socket;
    if (!socket) {
        return -1; // Invalid
    }

    pomelo_list_t * sessions = socket->sessions;
    if (!sessions || iterator->signature != sessions->mod_count) {
        return -1; // Invalid
    }

    iterator->state = entry->next;
    *p_session = session;
    return 0;
}


size_t pomelo_session_iterator_load(
    pomelo_session_iterator_t * iterator,
    size_t max_elements,
    pomelo_session_t ** array_sessions
) {
    assert(iterator != NULL);
    assert(array_sessions != NULL);

    pomelo_list_entry_t * entry = iterator->state;
    if (!entry) return 0; // No more elements

    pomelo_session_t * session = pomelo_list_element(entry, pomelo_session_t *);
    pomelo_socket_t * socket = session->socket;
    if (!socket) return 0;

    pomelo_list_t * sessions = socket->sessions;
    if (!sessions || iterator->signature != sessions->mod_count) return 0;

    size_t count = 0;
    while (count < max_elements && session) {
        array_sessions[count] = session;
        count++;
        entry = entry->next;
        session = pomelo_list_element(entry, pomelo_session_t *);
    }

    iterator->state = entry;
    return count;
}


void pomelo_socket_destroy_deferred(pomelo_socket_t * socket) {
    assert(socket != NULL);

    // Dispatch to plugins
    pomelo_plugin_dispatch_socket_on_destroyed(socket);
    pomelo_pool_release(socket->context->socket_pool, socket);
}


void pomelo_socket_stop_deferred(pomelo_socket_t * socket) {
    assert(socket != NULL);
    socket->state = POMELO_SOCKET_STATE_STOPPED;

    // Stop the socket
    pomelo_protocol_socket_stop(socket->protocol_socket);

    // Dispatch stop event to plugins
    pomelo_plugin_stop_socket(socket);

    // Release all sessions and clear the sessions list
    pomelo_session_t * session = NULL;
    pomelo_list_t * sessions = socket->sessions;
    pomelo_context_t * context = socket->context;

    pomelo_pool_t * builtin_pool = context->builtin_session_pool;
    pomelo_pool_t * plugin_pool = context->plugin_session_pool;
    while (pomelo_list_pop_front(sessions, &session) == 0) {
        switch (session->type) {
            case POMELO_SESSION_TYPE_BUILTIN:
                pomelo_pool_release(builtin_pool, session);
                break;
            case POMELO_SESSION_TYPE_PLUGIN:
                pomelo_pool_release(plugin_pool, session);
                break;
            default:
                assert(0); // Invalid session type
        }
    }

    // Clear the extra data
    pomelo_protocol_socket_set_extra(socket->protocol_socket, NULL);

    // Destroy the protocol socket
    pomelo_protocol_socket_destroy(socket->protocol_socket);
    socket->protocol_socket = NULL;
    
    // Clear the private key
    memset(socket->private_key, 0, POMELO_KEY_BYTES);
}
