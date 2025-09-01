#include <assert.h>
#include <string.h>
#include "sender.h"
#include "socket.h"
#include "context.h"


/// @brief The tasks of senders
static pomelo_pipeline_entry_fn sender_tasks[] = {
    (pomelo_pipeline_entry_fn) pomelo_protocol_sender_process,
    (pomelo_pipeline_entry_fn) pomelo_protocol_sender_dispatch,
    (pomelo_pipeline_entry_fn) pomelo_protocol_sender_complete
};


/// @brief The worker requirements for each packet type
static bool worker_required[] = {
    [POMELO_PROTOCOL_PACKET_REQUEST]    = false,
    [POMELO_PROTOCOL_PACKET_DENIED]     = false,
    [POMELO_PROTOCOL_PACKET_CHALLENGE]  = true,
    [POMELO_PROTOCOL_PACKET_RESPONSE]   = false,
    [POMELO_PROTOCOL_PACKET_KEEP_ALIVE] = false,
    [POMELO_PROTOCOL_PACKET_PAYLOAD]    = false,
    [POMELO_PROTOCOL_PACKET_DISCONNECT] = false,
};



int pomelo_protocol_sender_init(
    pomelo_protocol_sender_t * sender,
    pomelo_protocol_sender_info_t * info
) {
    assert(sender != NULL);
    assert(info != NULL);

    pomelo_protocol_peer_t * peer = info->peer;
    pomelo_protocol_packet_t * packet = info->packet;
    uint32_t flags = info->flags;

    pomelo_protocol_socket_t * socket = peer->socket;
    pomelo_protocol_context_t * context = socket->context;

    sender->platform = socket->platform;
    sender->context = context;
    sender->socket = socket;
    sender->peer = peer;
    sender->flags = flags;
    sender->codec_ctx = peer->crypto_ctx;
    pomelo_protocol_crypto_context_ref(sender->codec_ctx);
    memset(&sender->task, 0, sizeof(sender->task));

    sender->packet = packet;

    // Initialize the pipeline
    pomelo_pipeline_options_t pipeline_options = {
        .tasks = sender_tasks,
        .task_count = sizeof(sender_tasks) / sizeof(sender_tasks[0]),
        .callback_data = sender,
        .sequencer = socket->sequencer
    };
    int ret = pomelo_pipeline_init(&sender->pipeline, &pipeline_options);
    if (ret < 0) return ret; // Failed to initialize pipeline

    // Acquire new buffer for encrypted view
    pomelo_buffer_t * buffer =
        pomelo_buffer_context_acquire(context->buffer_context);
    if (!buffer) return -1; // Failed to acquire buffer

    sender->view.buffer = buffer;
    sender->view.offset = 0;
    sender->view.length = 0;

    // Add this sender to entry of peer's sending senders list
    sender->entry = pomelo_list_push_back(peer->senders, sender);
    if (!sender->entry) return -1; // Failed to append to list

    return 0;
}


void pomelo_protocol_sender_cleanup(pomelo_protocol_sender_t * sender) {
    assert(sender != NULL);
    pomelo_pipeline_cleanup(&sender->pipeline);

    if (sender->codec_ctx) {
        pomelo_protocol_crypto_context_unref(sender->codec_ctx);
        sender->codec_ctx = NULL;
    }

    if (sender->entry) {
        assert(sender->peer != NULL);
        pomelo_list_remove(sender->peer->senders, sender->entry);
        sender->entry = NULL;
    }

    if (sender->view.buffer) {
        pomelo_buffer_unref(sender->view.buffer);
        sender->view.buffer = NULL;
    }

    if (sender->packet) {
        pomelo_protocol_context_release_packet(sender->context, sender->packet);
        sender->packet = NULL;
    }
}


void pomelo_protocol_sender_submit(pomelo_protocol_sender_t * sender) {
    assert(sender != NULL);
    // Start the pipeline
    pomelo_pipeline_start(&sender->pipeline);
}


/// @brief Process callback of sender
static void sender_process_entry(pomelo_protocol_sender_t * sender) {
    assert(sender != NULL);
    pomelo_protocol_packet_t * packet = sender->packet;
    pomelo_buffer_view_t * view = &sender->view;
    pomelo_protocol_crypto_context_t * codec_ctx = sender->codec_ctx;

    // Make packet header
    pomelo_protocol_packet_header_t header;
    pomelo_protocol_packet_header_init(&header, packet);

    // Encode header first
    int ret = pomelo_protocol_packet_header_encode(&header, view);
    if (ret < 0) {
        sender->process_result = ret;
        return; // Failed to encode packet header
    }

    pomelo_buffer_view_t body_view;
    body_view.buffer = view->buffer;
    body_view.offset = view->offset + view->length; // Skip header
    body_view.length = 0;

    // Then encode body.
    ret = pomelo_protocol_packet_encode(packet, codec_ctx, &body_view);
    if (ret < 0) {
        sender->process_result = ret;
        return; // Failed to encode packet body
    }

    if (!(sender->flags & POMELO_PROTOCOL_SENDER_FLAG_NO_ENCRYPT)) {
        // Encrypt the packet body
        ret = pomelo_protocol_crypto_context_encrypt_packet(
            codec_ctx, &body_view, &header
        );
        if (ret < 0) {
            sender->process_result = ret;
            return; // Failed to encrypt packet body
        }
    }

    // Update the original view
    view->length += body_view.length;
    sender->process_result = 0;
}


/// @brief Complete callback of sender
static void sender_process_complete(
    pomelo_protocol_sender_t * sender,
    uint32_t flags
) {
    assert(sender != NULL);

    if (flags & POMELO_PLATFORM_TASK_FLAG_CANCELED) {
        sender->flags |= POMELO_PROTOCOL_SENDER_FLAG_CANCELED;
    }

    if (sender->process_result < 0) {
        sender->flags |= POMELO_PROTOCOL_SENDER_FLAG_FAILED;
    }

    if (sender->flags & POMELO_PROTOCOL_SENDER_FLAG_CANCELED ||
        sender->flags & POMELO_PROTOCOL_SENDER_FLAG_FAILED
    ) {
        pomelo_pipeline_finish(&sender->pipeline);
        return;
    }

    // Next stage
    pomelo_pipeline_next(&sender->pipeline);
}


void pomelo_protocol_sender_process(pomelo_protocol_sender_t * sender) {
    assert(sender != NULL);
    if (sender->flags & POMELO_PROTOCOL_SENDER_FLAG_NO_ENCRYPT) {
        // Packet is not encrypted
        if (!worker_required[sender->packet->type]) {
            // No worker encoding is required, process directly
            sender_process_entry(sender);
            sender_process_complete(sender, false);
            return;
        }
    }

    // Submit task to worker
    int ret = pomelo_platform_submit_worker_task(
        sender->platform,
        (pomelo_platform_task_entry) sender_process_entry,
        (pomelo_platform_task_complete) sender_process_complete,
        sender,
        &sender->task
    );

    if (ret < 0) {
        // Failed to submit to worker
        sender->flags |= POMELO_PROTOCOL_SENDER_FLAG_FAILED;
        pomelo_pipeline_finish(&sender->pipeline);
        return;
    }
}


void pomelo_protocol_sender_dispatch(pomelo_protocol_sender_t * sender) {
    assert(sender != NULL);

    // Address only available for server
    pomelo_address_t * address = NULL;
    pomelo_protocol_socket_t * socket = sender->socket;
    if (socket->mode == POMELO_PROTOCOL_SOCKET_MODE_SERVER) {
        address = &sender->peer->address;
    }

    int ret = pomelo_adapter_send(
        socket->adapter,
        address,
        &sender->view,
        !(sender->flags & POMELO_PROTOCOL_SENDER_FLAG_NO_ENCRYPT)
    );

    if (ret < 0) {
        // Failed to dispatch
        sender->flags |= POMELO_PROTOCOL_SENDER_FLAG_FAILED;
        pomelo_pipeline_finish(&sender->pipeline);
        return;
    }

    // Next stage
    pomelo_pipeline_next(&sender->pipeline);
}


void pomelo_protocol_sender_complete(pomelo_protocol_sender_t * sender) {
    assert(sender != NULL);
    pomelo_protocol_context_t * context = sender->context;

    if (sender->flags & POMELO_PROTOCOL_SENDER_FLAG_CANCELED) {
        // Sender has been canceled, release itself
        pomelo_pool_release(context->sender_pool, sender);
        return;
    }

    // Socket handles the sender
    pomelo_protocol_socket_handle_sender_complete(sender->socket, sender);

    // Release the sender
    pomelo_pool_release(context->sender_pool, sender);
}


void pomelo_protocol_sender_cancel(pomelo_protocol_sender_t * sender) {
    assert(sender != NULL);
    if (sender->flags & POMELO_PROTOCOL_SENDER_FLAG_CANCELED) {
        return; // Sender has been canceled, ignore
    }
    sender->flags |= POMELO_PROTOCOL_SENDER_FLAG_CANCELED;

    // Cancel the task (if any)
    pomelo_platform_cancel_worker_task(sender->platform, &sender->task);

    // Remove the sender from the peer's sending senders list
    if (sender->entry && sender->peer) {
        pomelo_list_remove(sender->peer->senders, sender->entry);
    }
    sender->peer = NULL;
    sender->entry = NULL;
}
