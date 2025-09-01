#include <assert.h>
#include <string.h>
#include "endpoint.h"
#include "bus.h"
#include "parcel.h"
#include "receiver.h"
#include "context.h"

/// The maximum alive time of unreliable parcel. The parcel will be
/// auto-released after a certain time but not great than this value.
#define POMELO_UNREL_MSG_MAX_ALIVE_TIME_NS 1000000000ULL // 1s

/// The minimum alive time of unreliable parcel. The parcel will be
/// auto-released after a certain time but not less than this value.
#define POMELO_UNREL_MSG_MIN_ALIVE_TIME_NS 100000000ULL    // 100ms


/// The factor of expired time for unreliable parcel:
/// expired = factor * rtt
#define POMELO_UNREL_MSG_EXPIRED_FACTOR_RTT 10

/// The initial capacity of the fragments array
#define POMELO_DELIVERY_RECEIVER_FRAGMENTS_INIT_CAPACITY 16


/// @brief The tasks of the receiving command
static pomelo_pipeline_entry_fn receiver_tasks[] = {
    (pomelo_pipeline_entry_fn) pomelo_delivery_receiver_wait_fragments,
    (pomelo_pipeline_entry_fn) pomelo_delivery_receiver_verify_checksum,
    (pomelo_pipeline_entry_fn) pomelo_delivery_receiver_complete,
};


/* -------------------------------------------------------------------------- */
/*                             Receiving Command                              */
/* -------------------------------------------------------------------------- */


int pomelo_delivery_receiver_on_alloc(
    pomelo_delivery_receiver_t * receiver,
    pomelo_delivery_context_t * context
) {
    assert(receiver != NULL);
    assert(context != NULL);

    pomelo_array_options_t array_options = {
        .allocator = context->allocator,
        .element_size = sizeof(pomelo_delivery_fragment_t),
        .initial_capacity = POMELO_DELIVERY_RECEIVER_FRAGMENTS_INIT_CAPACITY
    };
    receiver->fragments = pomelo_array_create(&array_options);
    if (!receiver->fragments) return -1; // Failed to create new array

    return 0;
}


/// @brief The on free function of the receiving command
void pomelo_delivery_receiver_on_free(pomelo_delivery_receiver_t * receiver) {
    assert(receiver != NULL);
    
    if (receiver->fragments) {
        pomelo_array_destroy(receiver->fragments);
        receiver->fragments = NULL;
    }
    
    return;
}


int pomelo_delivery_receiver_init(
    pomelo_delivery_receiver_t * receiver,
    pomelo_delivery_receiver_info_t * info
) {
    assert(receiver != NULL);
    assert(info != NULL);

    pomelo_delivery_bus_t * bus = info->bus;
    pomelo_delivery_fragment_meta_t * meta = info->meta;
    pomelo_delivery_context_t * context = bus->context;
    pomelo_delivery_endpoint_t * endpoint = bus->endpoint;

    // Initialize the pipeline
    pomelo_pipeline_options_t pipeline_options = {
        .tasks = receiver_tasks,
        .task_count = sizeof(receiver_tasks) / sizeof(receiver_tasks[0]),
        .callback_data = receiver,
        .sequencer = endpoint->sequencer
    };
    int ret = pomelo_pipeline_init(&receiver->pipeline, &pipeline_options);
    if (ret < 0) return ret; // Failed to initialize pipeline

    receiver->context = context;
    receiver->platform = endpoint->platform;
    receiver->bus = bus;
    receiver->mode = pomelo_delivery_mode_from_fragment_type(meta->type);
    receiver->sequence = meta->sequence;
    receiver->recv_fragments = 0;
    receiver->expired_time = 0;
    receiver->expired_entry = NULL;
    receiver->sequence_entry = NULL;
    receiver->flags = 0;
    memset(
        &receiver->checksum_verify_task,
        0,
        sizeof(receiver->checksum_verify_task)
    );
    receiver->checksum_compute_result = 0;

    // Add command to map
    receiver->sequence_entry = pomelo_map_set(
        bus->receivers_map,
        receiver->sequence,
        receiver
    );
    if (!receiver->sequence_entry) return -1; // Cannot set command to map

    // Reserve room for fragments
    size_t nfragments = meta->last_index + 1;
    ret = pomelo_array_resize(receiver->fragments, nfragments);
    if (ret < 0) return ret;

    for (size_t i = 0; i < nfragments; i++) {
        pomelo_delivery_fragment_t * fragment =
            pomelo_array_get_ptr(receiver->fragments, i);
        assert(fragment != NULL);
        pomelo_delivery_fragment_init(fragment);
    }

    return 0;
}


void pomelo_delivery_receiver_cleanup(pomelo_delivery_receiver_t * receiver) {
    assert(receiver != NULL);
    pomelo_pipeline_cleanup(&receiver->pipeline);

    // Cleanup the fragments
    pomelo_array_t * fragments = receiver->fragments;
    size_t nfragments = fragments->size;
    for (size_t i = 0; i < nfragments; i++) {
        pomelo_delivery_fragment_t * fragment =
            pomelo_array_get_ptr(fragments, i);
        assert(fragment != NULL);
        pomelo_delivery_fragment_cleanup(fragment);
    }
    pomelo_array_clear(fragments);

    // Cleanup the expired entry
    if (receiver->expired_entry) {
        assert(receiver->bus != NULL);
        pomelo_heap_remove(
            receiver->bus->receivers_heap,
            receiver->expired_entry
        );
        receiver->expired_entry = NULL;
    }
    
    // Cleanup the sequence entry
    if (receiver->sequence_entry) {
        assert(receiver->bus != NULL);
        pomelo_map_remove(
            receiver->bus->receivers_map,
            receiver->sequence_entry
        );
        receiver->sequence_entry = NULL;
    }
}


void pomelo_delivery_receiver_submit(pomelo_delivery_receiver_t * receiver) {
    assert(receiver != NULL);
    // Start the pipeline
    pomelo_pipeline_start(&receiver->pipeline);
}


int pomelo_delivery_receiver_check_meta(
    pomelo_delivery_receiver_t * receiver,
    pomelo_delivery_fragment_meta_t * meta
) {
    assert(receiver != NULL);
    assert(meta != NULL);

    // Check the sequence of the parcel
    if (receiver->sequence != meta->sequence) return -1;
    
    // Check the mode of the parcel
    if (receiver->mode != pomelo_delivery_mode_from_fragment_type(meta->type)) {
        return -1;
    }

    // The number of fragments in the receiving parcel must be the same as the
    // total fragments in the fragment meta
    if (receiver->fragments->size != (meta->last_index + 1)) return -1;

    return 0;
}


void pomelo_delivery_receiver_cancel(pomelo_delivery_receiver_t * receiver) {
    assert(receiver != NULL);
    if (receiver->flags & POMELO_DELIVERY_RECEIVER_FLAG_CANCELED) return;
    receiver->flags |= POMELO_DELIVERY_RECEIVER_FLAG_CANCELED;

    pomelo_delivery_bus_t * bus = receiver->bus;
    if (bus) {
        if (receiver->expired_entry) {
            pomelo_heap_remove(bus->receivers_heap, receiver->expired_entry);
        }

        if (receiver->sequence_entry) {
            pomelo_map_remove(bus->receivers_map, receiver->sequence_entry);
        }
    }

    receiver->bus = NULL;
    receiver->expired_entry = NULL;
    receiver->sequence_entry = NULL;

    // Cancel the checksum verification task (if any)
    pomelo_platform_cancel_worker_task(
        receiver->platform,
        &receiver->checksum_verify_task
    );

    if (!(receiver->flags & POMELO_DELIVERY_RECEIVER_FLAG_COMPUTING_CHECKSUM)) {
        // Verification task has not been submitted, call the callback directly
        pomelo_delivery_receiver_complete(receiver);
    }
}


void pomelo_delivery_receiver_wait_fragments(
    pomelo_delivery_receiver_t * receiver
) {
    assert(receiver != NULL);
    pomelo_delivery_bus_t * bus = receiver->bus;

    if (receiver->mode == POMELO_DELIVERY_MODE_RELIABLE) {
        // With reliable mode, wait forever
        receiver->expired_time = 0;
        return;
    }

    // Schedule expired time of command
    uint64_t rtt_mean = 0;
    pomelo_delivery_endpoint_rtt(bus->endpoint, &rtt_mean, NULL);

    // Sequenced & unreliable
    uint64_t now = pomelo_platform_hrtime(receiver->platform);
    uint64_t time_ns = rtt_mean * POMELO_UNREL_MSG_EXPIRED_FACTOR_RTT;
    if (time_ns < POMELO_UNREL_MSG_MIN_ALIVE_TIME_NS) {
        time_ns = POMELO_UNREL_MSG_MIN_ALIVE_TIME_NS;
    } else if (time_ns > POMELO_UNREL_MSG_MAX_ALIVE_TIME_NS) {
        time_ns = POMELO_UNREL_MSG_MAX_ALIVE_TIME_NS;
    }

    receiver->expired_time = now + time_ns;
    receiver->expired_entry = pomelo_heap_push(bus->receivers_heap, receiver);
    if (!receiver->expired_entry) {
        receiver->flags |= POMELO_DELIVERY_RECEIVER_FLAG_FAILED;
        pomelo_pipeline_finish(&receiver->pipeline);
        return; // Failed to add command to heap
    }
}


void pomelo_delivery_receiver_add_fragment(
    pomelo_delivery_receiver_t * receiver,
    pomelo_delivery_fragment_meta_t * meta,
    pomelo_buffer_view_t * content
) {
    assert(receiver != NULL);
    assert(meta != NULL);
    assert(content != NULL);

    pomelo_array_t * fragments = receiver->fragments;
    pomelo_delivery_fragment_t * fragment =
        pomelo_array_get_ptr(fragments, meta->fragment_index);
    assert(fragment != NULL);
    if (fragment->content.buffer) return; // Received

    // Attach the content to the fragment
    pomelo_delivery_fragment_attach_content(fragment, content);

    // Increment the received fragments counter
    receiver->recv_fragments++;
    if (receiver->recv_fragments == receiver->fragments->size) {
        // Received all fragments, next stage
        pomelo_pipeline_next(&receiver->pipeline);
    }
}


/// @brief Process the checksum verification task
static void verify_checksum_entry(pomelo_delivery_receiver_t * receiver) {
    assert(receiver != NULL);

    pomelo_crypto_checksum_state_t state;
    int ret = pomelo_crypto_checksum_init(&state);
    if (ret < 0) {
        receiver->checksum_compute_result = ret;
        return;
    }

    pomelo_array_t * fragments = receiver->fragments;
    size_t nfragments = fragments->size;
    
    for (size_t i = 0; i < nfragments; i++) {
        pomelo_delivery_fragment_t * fragment =
            pomelo_array_get_ptr(fragments, i);
        assert(fragment != NULL);
        pomelo_buffer_view_t * content = &fragment->content;
        if (!content->buffer) continue; // Skip the empty fragment

        ret = pomelo_crypto_checksum_update(
            &state,
            content->buffer->data + content->offset,
            content->length
        );
        if (ret < 0) {
            receiver->checksum_compute_result = ret;
            return;
        }
    }

    ret = pomelo_crypto_checksum_final(&state, receiver->computed_checksum);
    if (ret < 0) {
        receiver->checksum_compute_result = ret;
        return;
    }
}


/// @brief Complete the checksum verification task
static void verify_checksum_complete(
    pomelo_delivery_receiver_t * receiver,
    uint32_t flags
) {
    assert(receiver != NULL);

    receiver->flags &= ~POMELO_DELIVERY_RECEIVER_FLAG_COMPUTING_CHECKSUM;
    if (flags & POMELO_PLATFORM_TASK_FLAG_CANCELED) {
        receiver->flags |= POMELO_DELIVERY_RECEIVER_FLAG_CANCELED;
    }

    if (receiver->checksum_compute_result < 0) {
        receiver->flags |= POMELO_DELIVERY_RECEIVER_FLAG_FAILED;
    }

    if (receiver->flags & POMELO_DELIVERY_RECEIVER_FLAG_CANCELED ||
        receiver->flags & POMELO_DELIVERY_RECEIVER_FLAG_FAILED
    ) {
        // Failed
        pomelo_pipeline_finish(&receiver->pipeline);
        return;
    }

    // Compare the checksum
    int ret = memcmp(
        receiver->computed_checksum,
        receiver->embedded_checksum,
        POMELO_CRYPTO_CHECKSUM_BYTES
    );
    if (ret != 0) {
        // The checksum is not correct
        receiver->flags |= POMELO_DELIVERY_RECEIVER_FLAG_FAILED;
        pomelo_pipeline_finish(&receiver->pipeline);
        return;
    }

    // Checksum complete
    pomelo_pipeline_next(&receiver->pipeline);
}


void pomelo_delivery_receiver_verify_checksum(
    pomelo_delivery_receiver_t * receiver
) {
    assert(receiver != NULL);

    // Slice the checksum from the last fragment
    pomelo_array_t * fragments = receiver->fragments;
    if (fragments->size < 2) {
        // Skip checksum verification
        pomelo_pipeline_next(&receiver->pipeline);
        return;
    }

    pomelo_delivery_fragment_t * fragment =
        pomelo_array_get_ptr(fragments, fragments->size - 1);
    assert(fragment != NULL);

    if (fragment->content.length < POMELO_CRYPTO_CHECKSUM_BYTES) {
        // Not enough data
        receiver->flags |= POMELO_DELIVERY_RECEIVER_FLAG_FAILED;
        pomelo_pipeline_finish(&receiver->pipeline);
        return;
    }

    // Mark the checksum in the last fragment
    pomelo_buffer_view_t * content = &fragment->content;
    size_t checksum_offset =
        content->offset + content->length - POMELO_CRYPTO_CHECKSUM_BYTES;
    receiver->embedded_checksum = content->buffer->data + checksum_offset;
    content->length -= POMELO_CRYPTO_CHECKSUM_BYTES;

    // Submit the checksum verification task
    receiver->flags |= POMELO_DELIVERY_RECEIVER_FLAG_COMPUTING_CHECKSUM;
    int ret = pomelo_platform_submit_worker_task(
        receiver->platform,
        (pomelo_platform_task_entry) verify_checksum_entry,
        (pomelo_platform_task_complete) verify_checksum_complete,
        receiver,
        &receiver->checksum_verify_task
    );

    if (ret < 0) {
        // Failed to submit the checksum verification task
        receiver->flags |= POMELO_DELIVERY_RECEIVER_FLAG_FAILED;
        receiver->flags &= ~POMELO_DELIVERY_RECEIVER_FLAG_COMPUTING_CHECKSUM;
        pomelo_pipeline_finish(&receiver->pipeline);
        return;
    }
}


void pomelo_delivery_receiver_complete(pomelo_delivery_receiver_t * receiver) {
    assert(receiver != NULL);
    pomelo_delivery_context_t * context = receiver->context;

    if (receiver->flags & POMELO_DELIVERY_RECEIVER_FLAG_CANCELED) {
        // The command has been canceled, release itself
        pomelo_pool_release(context->receiver_pool, receiver);
        return;
    }

    pomelo_delivery_bus_t * bus = receiver->bus;
    assert(bus != NULL);

    // Remove the command from the heap and map
    if (receiver->expired_entry) {
        pomelo_heap_remove(bus->receivers_heap, receiver->expired_entry);
        receiver->expired_entry = NULL;
    }

    assert(receiver->sequence_entry != NULL);
    pomelo_map_remove(bus->receivers_map, receiver->sequence_entry);
    receiver->sequence_entry = NULL;

    pomelo_delivery_bus_handle_receiver_complete(receiver->bus, receiver);
    pomelo_pool_release(context->receiver_pool, receiver);
}


int pomelo_delivery_receiver_compare_expiration(
    pomelo_delivery_receiver_t * receiver_a,
    pomelo_delivery_receiver_t * receiver_b
) {
    assert(receiver_a != NULL);
    assert(receiver_b != NULL);

    if (receiver_a->expired_time > receiver_b->expired_time) return 1;
    if (receiver_a->expired_time < receiver_b->expired_time) return -1;
    return 0;
}
