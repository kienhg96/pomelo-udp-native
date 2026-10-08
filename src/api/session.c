#include <assert.h>
#include <string.h>
#include "pomelo/errno.h"
#include "delivery/delivery.h"
#include "session.h"
#include "message.h"
#include "socket.h"
#include "context.h"
#include "channel.h"


/* -------------------------------------------------------------------------- */
/*                               Public APIs                                  */
/* -------------------------------------------------------------------------- */


void pomelo_session_set_extra(pomelo_session_t * session, void * data) {
    assert(session != NULL);
    pomelo_extra_set(session->extra, data);
}


void * pomelo_session_get_extra(pomelo_session_t * session) {
    assert(session != NULL);
    return pomelo_extra_get(session->extra);
}


int64_t pomelo_session_get_client_id(pomelo_session_t * session) {
    assert(session != NULL);
    return session->client_id;
}


pomelo_address_t * pomelo_session_get_address(pomelo_session_t * session) {
    assert(session != NULL);
    return &session->address;
}


void pomelo_session_send(
    pomelo_session_t * session,
    size_t channel_index,
    pomelo_message_t * message,
    void * data
) {
    assert(session != NULL);
    assert(message != NULL);

    // Prepare the message for sending
    pomelo_message_prepare_send(message, data);

    uint64_t signature = pomelo_session_get_signature(session);
    if (signature == 0) {
        pomelo_socket_dispatch_send_result(session->socket, message);
        return; // Session has been released
    }

    // Get the channel
    pomelo_channel_t * channel =
        pomelo_session_get_channel(session, channel_index);
    if (!channel) {
        // Invalid channel: finish the send so the message ref/BUSY flag are
        // balanced and the send callback is still invoked (as a failure).
        pomelo_socket_dispatch_send_result(session->socket, message);
        return;
    }

    // And send through it
    assert(channel->methods != NULL && channel->methods->send != NULL);
    channel->methods->send(channel, message);
}


uint64_t pomelo_session_get_signature(pomelo_session_t * session) {
    assert(session != NULL);
    return pomelo_atomic_uint64_load(&session->signature);
}


pomelo_socket_t * pomelo_session_get_socket(pomelo_session_t * session) {
    assert(session != NULL);
    return session->socket;
}


int pomelo_session_disconnect(pomelo_session_t * session) {
    assert(session != NULL);
    pomelo_sequencer_submit(
        &session->socket->sequencer,
        &session->disconnect_task
    );

    // => pomelo_session_disconnect_deferred()
    return 0;
}


int pomelo_session_get_rtt(pomelo_session_t * session, pomelo_rtt_t * rtt) {
    assert(session != NULL);
    assert(rtt != NULL);

    pomelo_session_methods_t * methods = session->methods;
    if (!methods) {
        // Invalid session
        return POMELO_ERR_SESSION_INVALID;
    }

    assert(methods->get_rtt != NULL);
    return methods->get_rtt(session, &rtt->mean, &rtt->variance);
}


pomelo_channel_t * pomelo_session_get_channel(
    pomelo_session_t * session,
    size_t channel_index
) {
    assert(session != NULL);
    pomelo_session_methods_t * methods = session->methods;
    if (!methods) return NULL; // Invalid session
    assert(methods->get_channel != NULL);
    return methods->get_channel(session, channel_index);
}


int pomelo_session_set_channel_mode(
    pomelo_session_t * session,
    size_t channel_index,
    pomelo_channel_mode mode
) {
    assert(session != NULL);
    pomelo_channel_t * channel =
        pomelo_session_get_channel(session, channel_index);
    if (!channel) {
        return POMELO_ERR_CHANNEL_INVALID;
    }

    return pomelo_channel_set_mode(channel, mode);
}


pomelo_channel_mode pomelo_session_get_channel_mode(
    pomelo_session_t * session,
    size_t channel_index
) {
    assert(session != NULL);
    pomelo_channel_t * channel =
        pomelo_session_get_channel(session, channel_index);
    if (!channel) {
        return POMELO_CHANNEL_MODE_UNRELIABLE;
    }

    return pomelo_channel_get_mode(channel);
}


/* -------------------------------------------------------------------------- */
/*                               Private APIs                                 */
/* -------------------------------------------------------------------------- */


int pomelo_session_on_alloc(
    pomelo_session_t * session,
    pomelo_context_t * context
) {
    assert(session != NULL);
    memset(session, 0, sizeof(pomelo_session_t));
    session->context = context;
    return 0;
}


void pomelo_session_on_free(pomelo_session_t * session) {
    (void) session; // Nothing to do
}


int pomelo_session_init(
    pomelo_session_t * session,
    pomelo_session_info_t * info
) {
    assert(session != NULL);
    assert(info != NULL);

    pomelo_extra_set(session->extra, NULL);
    session->type = info->type;
    session->socket = info->socket;
    session->client_id = 0;
    memset(&session->address, 0, sizeof(pomelo_address_t));

    // Update the signature
    uint64_t signature = ++info->socket->session_signature_generator;
    pomelo_atomic_uint64_store(&session->signature, signature);

    session->methods = info->methods;
    session->entry = NULL;
    session->state = POMELO_SESSION_STATE_DISCONNECTED;

    // Initialize the disconnect task
    pomelo_sequencer_task_init(
        &session->disconnect_task,
        (pomelo_sequencer_callback) pomelo_session_disconnect_deferred,
        session
    );

    return 0;
}


void pomelo_session_cleanup(pomelo_session_t * session) {
    assert(session != NULL);

    // Call the cleanup callback
    pomelo_session_on_cleanup(session);

    session->socket = NULL;
    session->methods = NULL;

    // Reset the session signature to avoid sending
    pomelo_atomic_uint64_store(&session->signature, 0);
}


void pomelo_session_disconnect_deferred(pomelo_session_t * session) {
    assert(session != NULL);
    pomelo_session_methods_t * methods = session->methods;
    if (!methods) return; // Invalid session

    assert(methods->disconnect != NULL);
    methods->disconnect(session);
}
