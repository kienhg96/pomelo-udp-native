#include <assert.h>
#include <string.h>
#include "utils/pool.h"
#include "socket.h"
#include "receiver.h"
#include "server.h"
#include "context.h"


#define WORKER_DECODE_REQUIRED (1 << 0)
#define WORKER_ENCODE_REQUIRED (1 << 1)

/// @brief The worker requirements for each packet type
static bool worker_required[] = {
    [POMELO_PROTOCOL_PACKET_REQUEST]    = true,
    [POMELO_PROTOCOL_PACKET_DENIED]     = false,
    [POMELO_PROTOCOL_PACKET_CHALLENGE]  = true,
    [POMELO_PROTOCOL_PACKET_RESPONSE]   = true,
    [POMELO_PROTOCOL_PACKET_KEEP_ALIVE] = false,
    [POMELO_PROTOCOL_PACKET_PAYLOAD]    = false,
    [POMELO_PROTOCOL_PACKET_DISCONNECT] = false,
};


/// @brief The tasks of receivers
static pomelo_pipeline_entry_fn receiver_tasks[] = {
    (pomelo_pipeline_entry_fn) pomelo_protocol_receiver_process,
    (pomelo_pipeline_entry_fn) pomelo_protocol_receiver_complete
};


int pomelo_protocol_receiver_init(
    pomelo_protocol_receiver_t * receiver,
    pomelo_protocol_receiver_info_t * info
) {
    assert(receiver != NULL);
    assert(info != NULL);

    pomelo_protocol_peer_t * peer = info->peer;
    pomelo_protocol_socket_t * socket = peer->socket;
    pomelo_protocol_context_t * context = socket->context;

    receiver->platform = socket->platform;
    receiver->context = context;
    receiver->socket = socket;
    receiver->peer = peer;
    receiver->flags = info->flags;
    receiver->crypto_ctx = peer->crypto_ctx;
    pomelo_protocol_crypto_context_ref(receiver->crypto_ctx);
    memset(&receiver->task, 0, sizeof(receiver->task));

    receiver->body_view = *info->body_view;
    pomelo_buffer_ref(receiver->body_view.buffer);

    receiver->header = *info->header;
    receiver->recv_time = pomelo_platform_hrtime(socket->platform);

    // Initialize pipeline
    pomelo_pipeline_options_t pipeline_options = {
        .tasks = receiver_tasks,
        .task_count = sizeof(receiver_tasks) / sizeof(receiver_tasks[0]),
        .callback_data = receiver,
        .sequencer = socket->sequencer
    };
    int ret = pomelo_pipeline_init(&receiver->pipeline, &pipeline_options);
    if (ret < 0) return ret; // Failed to initialize pipeline

    // Append to receiving receivers list
    receiver->entry = pomelo_list_push_back(peer->receivers, receiver);
    if (!receiver->entry) return -1; // Failed to append to list

    // Acquire new packet for this receiver
    pomelo_protocol_packet_header_t * header = info->header;
    receiver->packet = pomelo_pool_acquire(
        context->packet_pools[header->type],
        NULL // No information for packet
    );
    if (!receiver->packet) return -1; // Failed to acquire packet

    // Update sequence number of packet
    receiver->packet->sequence = header->sequence;
    return 0;
}


void pomelo_protocol_receiver_cleanup(pomelo_protocol_receiver_t * receiver) {
    assert(receiver != NULL);
    pomelo_pipeline_cleanup(&receiver->pipeline);
    
    if (receiver->crypto_ctx) {
        pomelo_protocol_crypto_context_unref(receiver->crypto_ctx);
        receiver->crypto_ctx = NULL;
    }

    if (receiver->packet) {
        pomelo_protocol_context_release_packet(
            receiver->context,
            receiver->packet
        );
        receiver->packet = NULL;
    }

    if (receiver->body_view.buffer) {
        pomelo_buffer_unref(receiver->body_view.buffer);
        receiver->body_view.buffer = NULL;
    }

    if (receiver->entry) {
        assert(receiver->peer != NULL);
        pomelo_list_remove(receiver->peer->receivers, receiver->entry);
        receiver->entry = NULL;
    }
}


/// @brief Process callback of receiver
static void receiver_process_entry(pomelo_protocol_receiver_t * receiver) {
    assert(receiver != NULL);
    pomelo_buffer_view_t * body_view = &receiver->body_view;
    pomelo_protocol_crypto_context_t * crypto_ctx = receiver->crypto_ctx;

    if (!(receiver->flags & POMELO_PROTOCOL_RECEIVER_FLAG_NO_DECRYPT)) {
        // Decrypt the packet body
        int ret = pomelo_protocol_crypto_context_decrypt_packet(
            crypto_ctx,
            body_view,
            &receiver->header
        );
        if (ret < 0) {
            receiver->process_result = ret;
            return;
        }
    }

    // Update sequence number of packet
    receiver->packet->sequence = receiver->header.sequence;

    // Decode the packet body
    int ret = pomelo_protocol_packet_decode(
        receiver->packet,
        crypto_ctx,
        body_view
    );
    if (ret < 0) {
        receiver->process_result = ret;
        return;
    }

    receiver->process_result = 0;
}


/// @brief Complete callback of receiver
static void receiver_process_complete(
    pomelo_protocol_receiver_t * receiver,
    uint32_t flags
) {
    assert(receiver != NULL);

    if (flags & POMELO_PLATFORM_TASK_FLAG_CANCELED) {
        receiver->flags |= POMELO_PROTOCOL_RECEIVER_FLAG_CANCELED;
    }

    if (receiver->process_result < 0) {
        receiver->flags |= POMELO_PROTOCOL_RECEIVER_FLAG_FAILED;
    }

    if (receiver->flags & POMELO_PROTOCOL_RECEIVER_FLAG_CANCELED ||
        receiver->flags & POMELO_PROTOCOL_RECEIVER_FLAG_FAILED
    ) {
        pomelo_pipeline_finish(&receiver->pipeline);
        return; // Receiver has been canceled
    }

    // Next stage
    pomelo_pipeline_next(&receiver->pipeline);
}


void pomelo_protocol_receiver_submit(pomelo_protocol_receiver_t * receiver) {
    assert(receiver != NULL);
    // Start the pipeline
    pomelo_pipeline_start(&receiver->pipeline);
}


void pomelo_protocol_receiver_process(pomelo_protocol_receiver_t * receiver) {
    assert(receiver != NULL);

    // Update sequence number of packet
    pomelo_protocol_packet_t * packet = receiver->packet;
    packet->sequence = receiver->header.sequence;

    if (receiver->flags & POMELO_PROTOCOL_RECEIVER_FLAG_NO_DECRYPT) {
        // Packet is not encrypted
        if (!worker_required[packet->type]) {
            receiver_process_entry(receiver);
            receiver_process_complete(receiver, false);
            return;
        }
    }

    // Submit process to worker
    int ret = pomelo_platform_submit_worker_task(
        receiver->platform,
        (pomelo_platform_task_entry) receiver_process_entry,
        (pomelo_platform_task_complete) receiver_process_complete,
        receiver,
        &receiver->task
    );

    if (ret < 0) {
        // Failed to submit to worker
        receiver->flags |= POMELO_PROTOCOL_RECEIVER_FLAG_FAILED;
        pomelo_pipeline_finish(&receiver->pipeline);
        return;
    }
}


void pomelo_protocol_receiver_complete(pomelo_protocol_receiver_t * receiver) {
    assert(receiver != NULL);
    pomelo_protocol_context_t * context = receiver->context;

    if (receiver->flags & POMELO_PROTOCOL_RECEIVER_FLAG_CANCELED) {
        // Receiver has been canceled, release itself
        pomelo_pool_release(context->receiver_pool, receiver);
        return;
    }

    // Remove the receiver from the peer's receiving receivers list
    assert(receiver->peer != NULL);
    assert(receiver->entry != NULL);
    pomelo_list_remove(receiver->peer->receivers, receiver->entry);
    receiver->entry = NULL;

    // Socket handles the receiver
    pomelo_protocol_socket_handle_receiver_complete(receiver->socket, receiver);

    // Release the receiver
    pomelo_pool_release(context->receiver_pool, receiver);
}


void pomelo_protocol_receiver_cancel(pomelo_protocol_receiver_t * receiver) {
    assert(receiver != NULL);
    if (receiver->flags & POMELO_PROTOCOL_RECEIVER_FLAG_CANCELED) {
        return; // Receiver has been canceled, ignore
    }
    receiver->flags |= POMELO_PROTOCOL_RECEIVER_FLAG_CANCELED;

    // Cancel the task (if any)
    pomelo_platform_cancel_worker_task(receiver->platform, &receiver->task);

    // Remove the receiver from the peer's receiving receivers list
    if (receiver->entry && receiver->peer) {
        pomelo_list_remove(receiver->peer->receivers, receiver->entry);
    }
    receiver->entry = NULL;
    receiver->peer = NULL;
}
