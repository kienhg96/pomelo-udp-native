#include <assert.h>
#include <string.h>
#include "crypto/checksum.h"
#include "sender.h"
#include "bus.h"
#include "endpoint.h"
#include "parcel.h"
#include "context.h"
#include "dispatcher.h"


/// @brief The tasks of the sender
static pomelo_pipeline_entry_fn sender_tasks[] = {
    (pomelo_pipeline_entry_fn) pomelo_delivery_sender_update_checksum,
    (pomelo_pipeline_entry_fn) pomelo_delivery_sender_dispatch,
    (pomelo_pipeline_entry_fn) pomelo_delivery_sender_complete,
};


/* -------------------------------------------------------------------------- */
/*                           Sending Batch Command                            */
/* -------------------------------------------------------------------------- */

int pomelo_delivery_sender_on_alloc(
    pomelo_delivery_sender_t * command,
    pomelo_delivery_context_t * context
) {
    assert(command != NULL);
    assert(context != NULL);

    // Create the list of recipients
    pomelo_list_options_t list_options;
    memset(&list_options, 0, sizeof(list_options));
    list_options.allocator = context->allocator;
    list_options.element_size = sizeof(pomelo_delivery_transmission_t *);
    command->records = pomelo_list_create(&list_options);
    if (!command->records) return -1; // Failed to create the recipients list

    // Create the list of dispatchers
    memset(&list_options, 0, sizeof(list_options));
    list_options.allocator = context->allocator;
    list_options.element_size = sizeof(pomelo_delivery_dispatcher_t *);
    command->dispatchers = pomelo_list_create(&list_options);
    if (!command->dispatchers) return -1; // Failed to new list

    return 0;
}


void pomelo_delivery_sender_on_free(pomelo_delivery_sender_t * command) {
    assert(command != NULL);

    if (command->records) {
        pomelo_list_destroy(command->records);
        command->records = NULL;
    }

    if (command->dispatchers) {
        pomelo_list_destroy(command->dispatchers);
        command->dispatchers = NULL;
    }
}


int pomelo_delivery_sender_init(
    pomelo_delivery_sender_t * sender,
    pomelo_delivery_sender_options_t * info
) {
    assert(sender != NULL);
    assert(info != NULL);

    // Initialize the pipeline
    pomelo_pipeline_options_t pipeline_options = {
        .tasks = sender_tasks,
        .task_count = sizeof(sender_tasks) / sizeof(sender_tasks[0]),
        .callback_data = sender
    };
    int ret = pomelo_pipeline_init(&sender->pipeline, &pipeline_options);
    if (ret < 0) return ret;

    // Initialize the command
    pomelo_delivery_parcel_t * parcel = info->parcel;
    pomelo_delivery_context_t * context = info->context;

    sender->context = context;
    sender->platform = info->platform;
    sender->parcel = parcel;
    pomelo_delivery_parcel_ref(parcel);

    sender->completed_counter = 0;
    sender->success_counter = 0;
    sender->flags = 0;
    memset(
        &sender->checksum_update_task,
        0,
        sizeof(sender->checksum_update_task)
    );
    sender->checksum_compute_result = 0;

    if (parcel->chunks->size == 0) {
        return -1; // No chunks
    }

    if (parcel->chunks->size > 1) {
        sender->checksum =
            pomelo_buffer_context_acquire(context->buffer_context);
        if (!sender->checksum) return -1;
    } else {
        sender->checksum = NULL;
    }

    return 0;
}


void pomelo_delivery_sender_cleanup(pomelo_delivery_sender_t * sender) {
    assert(sender != NULL);
    if (sender->checksum) {
        pomelo_buffer_unref(sender->checksum);
        sender->checksum = NULL;
    }

    if (sender->parcel) {
        pomelo_delivery_parcel_unref(sender->parcel);
        sender->parcel = NULL;
    }

    // Release all transmissions
    pomelo_delivery_context_t * context = sender->context;
    pomelo_delivery_transmission_t * record = NULL;
    while (pomelo_list_pop_front(sender->records, &record) == 0) {
        pomelo_pool_release(context->transmission_pool, record);
    }

    // Clear the dispatchers
    pomelo_list_clear(sender->dispatchers);
}


pomelo_delivery_sender_t * pomelo_delivery_sender_create(
    pomelo_delivery_sender_options_t * options
) {
    assert(options != NULL);
    if (!options->context || !options->parcel || !options->platform) {
        return NULL; // Invalid options
    }
    return pomelo_pool_acquire(options->context->sender_pool, options);
}


void pomelo_delivery_sender_set_extra(
    pomelo_delivery_sender_t * sender,
    void * data
) {
    assert(sender != NULL);
    pomelo_extra_set(sender->extra, data);
}


void * pomelo_delivery_sender_get_extra(pomelo_delivery_sender_t * sender) {
    assert(sender != NULL);
    return pomelo_extra_get(sender->extra);
}


int pomelo_delivery_sender_add_transmission(
    pomelo_delivery_sender_t * sender,
    pomelo_delivery_bus_t * bus,
    pomelo_delivery_mode mode
) {
    assert(sender != NULL);
    assert(bus != NULL);

    pomelo_delivery_context_t * context = sender->context;
    pomelo_delivery_transmission_t * record =
        pomelo_pool_acquire(context->transmission_pool, NULL);
    if (!record) return -1;

    record->bus = bus;
    record->mode = mode;

    pomelo_list_entry_t * entry =
        pomelo_list_push_back(sender->records, record);
    if (!entry) {
        // Failed to add the recipient to the list
        pomelo_pool_release(context->transmission_pool, record);
        return -1;
    }

    return 0;
}


void pomelo_delivery_sender_submit(pomelo_delivery_sender_t * sender) {
    assert(sender != NULL);
    
    // Start the pipeline
    pomelo_pipeline_start(&sender->pipeline);
}


/// @brief Update the checksum of the parcel
static void update_checksum_entry(pomelo_delivery_sender_t * sender) {
    assert(sender != NULL);

    pomelo_crypto_checksum_state_t state;
    int ret = pomelo_crypto_checksum_init(&state);
    if (ret < 0) {
        sender->checksum_compute_result = ret;
        return;
    }

    pomelo_array_t * chunks = sender->parcel->chunks;
    size_t nchunks = chunks->size;
    
    for (size_t i = 0; i < nchunks; i++) {
        pomelo_buffer_view_t * chunk = pomelo_array_get_ptr(chunks, i);
        assert(chunk != NULL);

        ret = pomelo_crypto_checksum_update(
            &state,
            chunk->buffer->data + chunk->offset,
            chunk->length
        );
        if (ret < 0) {
            sender->checksum_compute_result = ret;
            return;
        }
    }

    ret = pomelo_crypto_checksum_final(&state, sender->checksum->data);
    if (ret < 0) {
        sender->checksum_compute_result = ret;
        return;
    }
}


/// @brief Handle the checksum update complete event
static void update_checksum_complete(
    pomelo_delivery_sender_t * sender,
    uint32_t flags
) {
    assert(sender != NULL);

    if (sender->checksum_compute_result < 0) {
        sender->flags |= POMELO_DELIVERY_SENDER_FLAG_FAILED;
    }

    if (flags & POMELO_PLATFORM_TASK_FLAG_CANCELED) {
        sender->flags |= POMELO_DELIVERY_SENDER_FLAG_CANCELED;
    }

    if (sender->flags & (
        POMELO_DELIVERY_SENDER_FLAG_FAILED |
        POMELO_DELIVERY_SENDER_FLAG_CANCELED
    )) {
        // The sender has failed or canceled, finish the pipeline
        pomelo_pipeline_finish(&sender->pipeline);
        return;
    }

    pomelo_pipeline_next(&sender->pipeline);
}


void pomelo_delivery_sender_update_checksum(pomelo_delivery_sender_t * sender) {
    assert(sender != NULL);
    if (sender->parcel->chunks->size < 2) {
        pomelo_pipeline_next(&sender->pipeline);
        return; // No checksum
    }

    // Submit the checksum update task
    int ret = pomelo_platform_submit_worker_task(
        sender->platform,
        (pomelo_platform_task_entry) update_checksum_entry,
        (pomelo_platform_task_complete) update_checksum_complete,
        sender,
        &sender->checksum_update_task
    );
    if (ret < 0) {
        // Failed to submit the checksum update task
        sender->flags |= POMELO_DELIVERY_SENDER_FLAG_FAILED;
        pomelo_pipeline_finish(&sender->pipeline);
    }
}


/// @brief Create and submit a dispatcher
static int create_and_submit_dispatcher(
    pomelo_delivery_sender_t * sender,
    pomelo_delivery_transmission_t * recipient
) {
    assert(sender != NULL);
    assert(recipient != NULL);

    pomelo_delivery_context_t * context = sender->context;
    pomelo_delivery_bus_t * bus = recipient->bus;
    pomelo_delivery_dispatcher_info_t info = {
        .sender = sender,
        .bus = bus,
        .parcel = sender->parcel,
        .sequence = ++bus->sequence_generator,
        .mode = recipient->mode
    };
    pomelo_delivery_dispatcher_t * dispatcher =
        pomelo_pool_acquire(context->dispatcher_pool, &info);
    if (!dispatcher) return -1;

    // Trigger the sending process
    pomelo_delivery_bus_process_sending(bus);
    return 0;
}


void pomelo_delivery_sender_dispatch(pomelo_delivery_sender_t * sender) {
    assert(sender != NULL);

    pomelo_list_iterator_t it;
    pomelo_list_iterator_init(&it, sender->records);
    pomelo_delivery_transmission_t * recipient = NULL;

    size_t failed_counter = 0;
    while (pomelo_list_iterator_next(&it, &recipient) == 0) {
        int ret = create_and_submit_dispatcher(sender, recipient);
        if (ret < 0) {
            failed_counter++;
        }
    }

    // Update the completed counter by failed counter
    if (failed_counter > 0) {
        sender->completed_counter += failed_counter;
        if (sender->completed_counter == sender->records->size) {
            // All buses have completed sending, but some sub-sending commands
            // have failed
            pomelo_pipeline_next(&sender->pipeline);
        }
    }
}


/// @brief Complete the sending batch command
void pomelo_delivery_sender_complete(pomelo_delivery_sender_t * sender) {
    assert(sender != NULL);
    pomelo_delivery_context_t * context = sender->context;
    if (sender->flags & POMELO_DELIVERY_SENDER_FLAG_CANCELED) {
        // The command has been canceled, release itself
        pomelo_pool_release(context->sender_pool, sender);
        return;
    }

    // Call the callback
    if (!(sender->flags & POMELO_DELIVERY_SENDER_FLAG_SYSTEM)) {
        // Not a system sender, call the callback
        pomelo_delivery_sender_on_result(
            sender,
            sender->parcel,
            sender->success_counter
        );
    }

    // Release the command
    pomelo_pool_release(context->sender_pool, sender);
}


void pomelo_delivery_sender_cancel(pomelo_delivery_sender_t * sender) {
    assert(sender != NULL);
    if (sender->flags & POMELO_DELIVERY_SENDER_FLAG_CANCELED) {
        return; // Already canceled
    }
    sender->flags |= POMELO_DELIVERY_SENDER_FLAG_CANCELED;

    // Cancel the checksum update task (if any)
    pomelo_platform_cancel_worker_task(
        sender->platform,
        &sender->checksum_update_task
    );

    pomelo_delivery_dispatcher_t * dispatcher = NULL;
    while (pomelo_list_pop_front(sender->dispatchers, &dispatcher) == 0) {
        dispatcher->sender_entry = NULL;
        pomelo_delivery_dispatcher_cancel(dispatcher);
    }

    pomelo_pipeline_finish(&sender->pipeline);
}


void pomelo_delivery_sender_on_dispatcher_result(
    pomelo_delivery_sender_t * sender,
    pomelo_delivery_dispatcher_t * dispatcher
) {
    assert(sender != NULL);
    assert(dispatcher != NULL);

    if (!(dispatcher->flags & (
        POMELO_DELIVERY_DISPATCHER_FLAG_FAILED |
        POMELO_DELIVERY_DISPATCHER_FLAG_CANCELED
    ))) {
        // Not failed or canceled, increment the success counter
        sender->success_counter++;
    }

    // Increment the completed counter
    sender->completed_counter++;
    if (sender->completed_counter == sender->records->size) {
        // All buses have completed sending
        pomelo_pipeline_next(&sender->pipeline);
    }
}
