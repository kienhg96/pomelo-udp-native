#include <assert.h>
#include <string.h>
#include "utils/macro.h"
#include "bus.h"
#include "endpoint.h"
#include "parcel.h"
#include "endpoint.h"
#include "context.h"
#include "receiver.h"
#include "sender.h"
#include "dispatcher.h"


/* -------------------------------------------------------------------------- */
/*                               Public APIs                                  */
/* -------------------------------------------------------------------------- */


void pomelo_delivery_bus_set_extra(
    pomelo_delivery_bus_t * bus,
    void * extra
) {
    assert(bus != NULL);
    pomelo_extra_set(bus->extra, extra);
}


void * pomelo_delivery_bus_get_extra(pomelo_delivery_bus_t * bus) {
    assert(bus != NULL);
    return pomelo_extra_get(bus->extra);
}


pomelo_delivery_endpoint_t * pomelo_delivery_bus_get_endpoint(
    pomelo_delivery_bus_t * bus
) {
    assert(bus != NULL);
    return bus->endpoint;
}


/* -------------------------------------------------------------------------- */
/*                               Private APIs                                 */
/* -------------------------------------------------------------------------- */


/// @brief Compare two receivers by their expiration time
static int receiver_expiration_compare(void * a, void * b) {
    assert(a != NULL);
    assert(b != NULL);

    return pomelo_delivery_receiver_compare_expiration(
        *((pomelo_delivery_receiver_t **) a),
        *((pomelo_delivery_receiver_t **) b)
    );
}


int pomelo_delivery_bus_on_alloc(
    pomelo_delivery_bus_t * bus,
    pomelo_delivery_context_t * context
) {
    assert(bus != NULL);
    assert(context != NULL);

    pomelo_allocator_t * allocator = context->allocator;
    bus->context = context;

    // Create pending send parcels list
    pomelo_list_options_t list_options = {
        .allocator = allocator,
        .element_size = sizeof(pomelo_delivery_dispatcher_t *)
    };
    bus->pending_dispatchers = pomelo_list_create(&list_options);
    if (!bus->pending_dispatchers) return -1;

    // Create incomplete receiving parcels map
    pomelo_map_options_t map_options = {
        .allocator = allocator,
        .key_size = sizeof(uint64_t),
        .value_size = sizeof(pomelo_delivery_receiver_t *)
    };
    bus->receivers_map = pomelo_map_create(&map_options);
    if (!bus->receivers_map) return -1;

    // Create the heap for receiving commands
    pomelo_heap_options_t heap_options = {
        .allocator = allocator,
        .compare = receiver_expiration_compare,
        .element_size = sizeof(pomelo_delivery_receiver_t *)
    };
    bus->receivers_heap = pomelo_heap_create(&heap_options);
    if (!bus->receivers_heap) return -1;

    return 0;
}


void pomelo_delivery_bus_on_free(pomelo_delivery_bus_t * bus) {
    assert(bus != NULL);
    
    // Cleanup all commands
    if (bus->pending_dispatchers) {
        pomelo_list_destroy(bus->pending_dispatchers);
        bus->pending_dispatchers = NULL;
    }

    if (bus->receivers_map) {
        pomelo_map_destroy(bus->receivers_map);
        bus->receivers_map = NULL;
    }

    if (bus->receivers_heap) {
        pomelo_heap_destroy(bus->receivers_heap);
        bus->receivers_heap = NULL;
    }
}


int pomelo_delivery_bus_init(
    pomelo_delivery_bus_t * bus,
    pomelo_delivery_bus_info_t * info
) {
    assert(bus != NULL);
    assert(info != NULL);

    bus->endpoint = info->endpoint;
    bus->id = info->id;
    bus->platform = info->endpoint->platform;

    // Initialize the send task
    pomelo_sequencer_task_init(
        &bus->send_task,
        (pomelo_sequencer_callback)
            pomelo_delivery_bus_process_sending_deferred,
        bus
    );

    return 0;
}


void pomelo_delivery_bus_cleanup(pomelo_delivery_bus_t * bus) {
    (void) bus; // Ingore
}


int pomelo_delivery_bus_start(pomelo_delivery_bus_t * bus) {
    (void) bus; // Ingore
    return 0;
}


void pomelo_delivery_bus_stop(pomelo_delivery_bus_t * bus) {
    assert(bus != NULL);
    if (bus->flags & POMELO_DELIVERY_BUS_FLAG_PROCESSING) {
        // Bus is busy, just set the stop flag
        bus->flags |= POMELO_DELIVERY_BUS_FLAG_STOP;
        return;
    }

    // Cleanup dispatchers
    pomelo_delivery_dispatcher_t * dispatcher = NULL;
    pomelo_list_t * dispatchers = bus->pending_dispatchers;
    while (pomelo_list_pop_front(dispatchers, &dispatcher) == 0) {
        pomelo_delivery_dispatcher_cancel(dispatcher);
    }

    // Cleanup receivers
    pomelo_map_iterator_t it;
    pomelo_map_entry_t * entry;
    pomelo_map_iterator_init(&it, bus->receivers_map);
    pomelo_delivery_receiver_t * receiver = NULL;
    while (pomelo_map_iterator_next(&it, &entry) == 0) { // OK
        receiver = pomelo_map_entry_value_ptr(entry);
        receiver->sequence_entry = NULL;
        pomelo_delivery_receiver_cancel(receiver);
    }
    pomelo_map_clear(bus->receivers_map);

    // Cleanup current reliable receiver
    bus->incomplete_reliable_receiver = NULL;

    // Cleanup current reliable dispatcher
    if (bus->incomplete_reliable_dispatcher) {
        pomelo_delivery_dispatcher_cancel(bus->incomplete_reliable_dispatcher);
        bus->incomplete_reliable_dispatcher = NULL;
    }

    // Cleanup receivers heap
    pomelo_heap_clear(bus->receivers_heap);

    // Cleanup other values
    bus->last_recv_reliable_sequence = 0;
    bus->sequence_generator = 0;
    bus->last_recv_sequenced_sequence = 0;
    bus->flags = 0;
}


/* -------------------------------------------------------------------------- */
/*                             Receiving Process                              */
/* -------------------------------------------------------------------------- */

int pomelo_delivery_bus_recv(
    pomelo_delivery_bus_t * bus,
    pomelo_delivery_fragment_meta_t * meta,
    pomelo_buffer_view_t * view_content
) {
    assert(meta != NULL);

    pomelo_delivery_reception_t * recv =
        pomelo_delivery_reception_init(bus, meta, view_content);
    if (!recv) return -1; // Failed to initialize the receiving info

    // Submit the receiving task
    pomelo_sequencer_submit(bus->endpoint->sequencer, &recv->task);

    // => pomelo_delivery_reception_execute()
    return 0;
}


int pomelo_delivery_bus_recv_fragment_data(
    pomelo_delivery_bus_t * bus,
    pomelo_delivery_fragment_meta_t * meta,
    pomelo_buffer_view_t * content
) {
    assert(bus != NULL);
    assert(meta != NULL);

    // Validate meta
    if (meta->type == POMELO_FRAGMENT_TYPE_DATA_RELIABLE) {
        if (bus->incomplete_reliable_receiver) {
            // There's a incomplete reliable receiver
            if (meta->sequence != bus->last_recv_reliable_sequence) {
                return -1; // Mismatch reliable parcel sequence
            }
        } else {
            // There's no incomplete reliable receiver
            if (meta->sequence == bus->last_recv_reliable_sequence) {
                // This fragment is a part of the most recent reliable parcel
                pomelo_delivery_bus_reply_ack(bus, meta);
                return 0;
            }
        }
    } else if (meta->type == POMELO_FRAGMENT_TYPE_DATA_SEQUENCED) {
        // For sequenced parcel
        if (meta->sequence < bus->last_recv_sequenced_sequence) {
            return -1; // Out of date
        }
    }
    
    // Get the receiver
    pomelo_delivery_receiver_t * receiver =
        pomelo_delivery_bus_ensure_recv_command(bus, meta);
    if (!receiver) return -1; // Failed to prepare receiving command

    if (receiver->mode == POMELO_DELIVERY_MODE_RELIABLE) {
        // Reply the ack to the sender
        pomelo_delivery_bus_reply_ack(bus, meta);
    }

    // Append the fragment to the command
    pomelo_delivery_receiver_add_fragment(receiver, meta, content);
    return 0;
}


void pomelo_delivery_bus_handle_receiver_complete(
    pomelo_delivery_bus_t * bus,
    pomelo_delivery_receiver_t * receiver
) {
    assert(bus != NULL);
    assert(receiver != NULL);

    if (receiver->mode == POMELO_DELIVERY_MODE_RELIABLE) {
        assert(bus->incomplete_reliable_receiver == receiver);
        bus->incomplete_reliable_receiver = NULL;
    }

    if (receiver->flags & POMELO_DELIVERY_RECEIVER_FLAG_FAILED) {
        return; // Failed, ignore
    }

    // Build the parcel
    pomelo_delivery_parcel_t * parcel =
        pomelo_delivery_context_acquire_parcel(bus->context);
    if (!parcel) return; // Failed to acquire parcel

    // Set the fragments
    int ret = pomelo_delivery_parcel_set_fragments(parcel, receiver->fragments);
    if (ret < 0) {
        pomelo_delivery_parcel_unref(parcel);
        return; // Failed to set fragments
    }

    if (receiver->mode != POMELO_DELIVERY_MODE_SEQUENCED) {
        // Just call the callback
        pomelo_delivery_bus_dispatch_received(bus, parcel, receiver->mode);
        pomelo_delivery_parcel_unref(parcel);
        return;
    }

    // For sequenced parcel, check the sequence
    if (receiver->sequence < bus->last_recv_sequenced_sequence) {
        return; // Out of date
    }
    bus->last_recv_sequenced_sequence = receiver->sequence;

    // Call the callback
    pomelo_delivery_bus_dispatch_received(bus, parcel, receiver->mode);
    pomelo_delivery_parcel_unref(parcel);
}


int pomelo_delivery_bus_recv_fragment_ack(
    pomelo_delivery_bus_t * bus,
    pomelo_delivery_fragment_meta_t * meta
) {
    assert(bus != NULL);
    assert(meta != NULL);

    // Check the current reliable sending command
    pomelo_delivery_dispatcher_t * dispatcher =
        bus->incomplete_reliable_dispatcher;
    if (!dispatcher || dispatcher->sequence != meta->sequence) {
        return -1; // Invalid ack
    }

    // Process ack
    pomelo_delivery_dispatcher_recv_ack(dispatcher, meta);
    return 0;
}


int pomelo_delivery_bus_reply_ack(
    pomelo_delivery_bus_t * bus,
    pomelo_delivery_fragment_meta_t * meta
) {
    assert(bus != NULL);
    assert(meta != NULL);

    pomelo_delivery_endpoint_t * endpoint = bus->endpoint;
    pomelo_delivery_context_t * context = endpoint->context;

    // Acquire new buffer for writing
    pomelo_buffer_t * buffer =
        pomelo_buffer_context_acquire(context->buffer_context);
    if (!buffer) return -1; // Failed to acquire buffer

    // Clone the meta and set the type to ack
    pomelo_delivery_fragment_meta_t ack_meta = *meta;
    ack_meta.type = POMELO_FRAGMENT_TYPE_ACK;

    pomelo_buffer_view_t view;
    view.buffer = buffer;
    view.offset = 0;
    view.length = 0;

    // Encode the meta at the beginning of the buffer
    int ret = pomelo_delivery_fragment_meta_encode(&ack_meta, &view);
    if (ret < 0) {
        // Failed to encode meta
        pomelo_buffer_unref(buffer);
        return ret;
    }

    // Send the payload
    ret = pomelo_delivery_endpoint_send(endpoint, &view, 1);

    // Finally, unref the payload
    pomelo_buffer_unref(buffer);
    return ret;
}


pomelo_delivery_receiver_t * pomelo_delivery_bus_ensure_recv_command(
    pomelo_delivery_bus_t * bus,
    pomelo_delivery_fragment_meta_t * meta
) {
    assert(bus != NULL);
    assert(meta != NULL);

    // Cleanup expired receivers first
    pomelo_delivery_bus_cleanup_expired_receivers(bus);

    // Get the receiver from map
    uint64_t sequence = meta->sequence;

    pomelo_delivery_receiver_t * receiver = NULL;
    pomelo_map_get(bus->receivers_map, sequence, &receiver);
    if (receiver) {
        int ret = pomelo_delivery_receiver_check_meta(receiver, meta);
        if (ret < 0) return NULL; // Invalid meta, discard
        return receiver;
    }

    // Acquire a receiver from context pool
    pomelo_delivery_receiver_info_t info = {
        .bus = bus,
        .meta = meta
    };
    receiver = pomelo_pool_acquire(bus->context->receiver_pool, &info);
    if (!receiver) return NULL; // Failed to acquire new receiver

    if (receiver->mode == POMELO_DELIVERY_MODE_RELIABLE) {
        assert(bus->incomplete_reliable_receiver == NULL);
        bus->incomplete_reliable_receiver = receiver;
        bus->last_recv_reliable_sequence = receiver->sequence;
    }

    // Submit the receiving command
    pomelo_delivery_receiver_submit(receiver);
    return receiver;
}


void pomelo_delivery_bus_cleanup_expired_receivers(
    pomelo_delivery_bus_t * bus
) {
    assert(bus != NULL);

    uint64_t now = pomelo_platform_hrtime(bus->platform);
    pomelo_heap_t * receivers = bus->receivers_heap;
    pomelo_delivery_receiver_t * command = NULL;

    while (pomelo_heap_top(receivers, &command) == 0) {
        if (command->expired_time > now) break;

        pomelo_heap_pop(receivers, NULL);
        command->expired_entry = NULL;
        pomelo_delivery_receiver_cancel(command);
    }
}


void pomelo_delivery_bus_dispatch_received(
    pomelo_delivery_bus_t * bus,
    pomelo_delivery_parcel_t * parcel,
    pomelo_delivery_mode mode
) {
    assert(bus != NULL);
    assert(parcel != NULL);

    if (bus == bus->endpoint->system_bus) {
        // The system bus
        pomelo_delivery_endpoint_recv_system_parcel(bus->endpoint, parcel);
    } else {
        // The user bus
        pomelo_delivery_bus_on_received(bus, parcel, mode);
    }
}


pomelo_delivery_reception_t * pomelo_delivery_reception_init(
    pomelo_delivery_bus_t * bus,
    pomelo_delivery_fragment_meta_t * meta,
    pomelo_buffer_view_t * view_content
) {
    assert(bus != NULL);
    assert(meta != NULL);
    assert(view_content != NULL);
    
    pomelo_pool_t * pool = bus->context->reception_pool;
    pomelo_delivery_reception_t * recv = pomelo_pool_acquire(pool, NULL);
    if (!recv) return NULL; // Failed to acquire recv info

    recv->bus = bus;
    recv->meta = *meta;
    recv->content = *view_content;
    pomelo_buffer_ref(view_content->buffer);

    // Initialize the task
    pomelo_sequencer_task_init(
        &recv->task,
        (pomelo_sequencer_callback) pomelo_delivery_reception_execute,
        recv
    );

    return recv;
}


void pomelo_delivery_reception_execute(pomelo_delivery_reception_t * recv) {
    assert(recv != NULL);

    pomelo_delivery_bus_t * bus = recv->bus;
    pomelo_delivery_fragment_meta_t * meta = &recv->meta;

    if (meta->type == POMELO_FRAGMENT_TYPE_ACK) {
        pomelo_delivery_bus_recv_fragment_ack(bus, meta);
    } else {
        pomelo_delivery_bus_recv_fragment_data(bus, meta, &recv->content);
    }

    // Release the content buffer
    pomelo_buffer_unref(recv->content.buffer);

    // Release the reception info
    pomelo_pool_release(recv->bus->context->reception_pool, recv);
}


/* -------------------------------------------------------------------------- */
/*                              Sending Process                               */
/* -------------------------------------------------------------------------- */


void pomelo_delivery_bus_process_sending(pomelo_delivery_bus_t * bus) {
    assert(bus != NULL);
    if (bus->flags & POMELO_DELIVERY_BUS_FLAG_PROCESSING) return;
    bus->flags |= POMELO_DELIVERY_BUS_FLAG_PROCESSING;

    // Submit the sending task
    pomelo_sequencer_submit(bus->endpoint->sequencer, &bus->send_task);
}


void pomelo_delivery_bus_process_sending_deferred(pomelo_delivery_bus_t * bus) {
    assert(bus != NULL);
    pomelo_list_t * dispatchers = bus->pending_dispatchers;

    // Reliable sending command will block the bus
    while (
        !(bus->flags & POMELO_DELIVERY_BUS_FLAG_STOP)
        && !bus->incomplete_reliable_dispatcher
    ) {
        pomelo_delivery_dispatcher_t * dispatcher = NULL;
        if (pomelo_list_pop_front(dispatchers, &dispatcher) != 0) break;

        if (dispatcher->mode == POMELO_DELIVERY_MODE_RELIABLE) {
            bus->incomplete_reliable_dispatcher = dispatcher;
        }

        pomelo_delivery_dispatcher_submit(dispatcher);
    }

    bus->flags &= ~POMELO_DELIVERY_BUS_FLAG_PROCESSING;

    // Check for stopping
    if (bus->flags & POMELO_DELIVERY_BUS_FLAG_STOP) {
        pomelo_delivery_bus_stop(bus);
    }
}


void pomelo_delivery_bus_on_dispatcher_completed(
    pomelo_delivery_bus_t * bus,
    pomelo_delivery_dispatcher_t * dispatcher
) {
    assert(bus != NULL);
    assert(dispatcher != NULL);

    if (dispatcher == bus->incomplete_reliable_dispatcher) {
        // The current reliable sending command is completed
        bus->incomplete_reliable_dispatcher = NULL;
    }

    // Continue processing.
    pomelo_delivery_bus_process_sending(bus);
}
