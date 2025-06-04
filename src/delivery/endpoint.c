#include <assert.h>
#include <string.h>
#include "utils/macro.h"
#include "utils/clz.h"
#include "endpoint.h"
#include "bus.h"
#include "parcel.h"
#include "context.h"
#include "sender.h"

#define POMELO_DELIVERY_ENDPOINT_DEFAULT_BUSES_CAPACITY 16


pomelo_delivery_endpoint_t * pomelo_delivery_endpoint_create(
    pomelo_delivery_endpoint_options_t * options
) {
    assert(options != NULL);
    if (!options->context || !options->heartbeat || options->nbuses == 0) {
        return NULL; // Invalid options
    }

    pomelo_delivery_context_t * context = options->context;
    return pomelo_pool_acquire(context->endpoint_pool, options);
}


void pomelo_delivery_endpoint_destroy(pomelo_delivery_endpoint_t * endpoint) {
    assert(endpoint != NULL);
    pomelo_sequencer_submit(endpoint->sequencer, &endpoint->destroy_task);
    // => pomelo_delivery_endpoint_destroy_deferred()
}


void * pomelo_delivery_endpoint_get_extra(
    pomelo_delivery_endpoint_t * endpoint
) {
    assert(endpoint != NULL);
    return pomelo_extra_get(endpoint->extra);
}


void pomelo_delivery_endpoint_set_extra(
    pomelo_delivery_endpoint_t * endpoint,
    void * data
) {
    assert(endpoint != NULL);
    pomelo_extra_set(endpoint->extra, data);
}


pomelo_delivery_bus_t * pomelo_delivery_endpoint_get_bus(
    pomelo_delivery_endpoint_t * endpoint,
    size_t index
) {
    assert(endpoint != NULL);
    assert(index < endpoint->buses->size);

    pomelo_delivery_bus_t * bus = NULL;
    pomelo_array_get(endpoint->buses, index, &bus);
    return bus;
}


int pomelo_delivery_endpoint_recv(
    pomelo_delivery_endpoint_t * endpoint,
    pomelo_buffer_view_t * view
) {
    assert(endpoint != NULL);
    assert(view != NULL);

    // Decode the meta
    pomelo_delivery_fragment_meta_t meta;
    int ret = pomelo_delivery_fragment_meta_decode(&meta, view);
    if (ret < 0) return ret; // Failed to decode the meta
    // View is now at the beginning of the content

    if (meta.last_index >= endpoint->context->max_fragments)  {
        return -1; // Exceed the maximum number of fragments
    }

    // Get the bus
    pomelo_delivery_bus_t * bus = NULL;
    if (meta.bus_id == 0) {
        bus = endpoint->system_bus;
    } else {
        if (!(endpoint->flags & POMELO_DELIVERY_ENDPOINT_FLAG_READY)) {
            return -1; // Endpoint is not ready
        }

        ret = pomelo_array_get(endpoint->buses, meta.bus_id - 1, &bus);
        if (ret < 0 || !bus) return -1; // Failed to get bus
    }

    return pomelo_delivery_bus_recv(bus, &meta, view);
}


void pomelo_delivery_endpoint_rtt(
    pomelo_delivery_endpoint_t * endpoint,
    uint64_t * mean,
    uint64_t * variance
) {
    assert(endpoint != NULL);
    pomelo_rtt_calculator_get(&endpoint->rtt, mean, variance);
}


int64_t pomelo_delivery_endpoint_time_offset(
    pomelo_delivery_endpoint_t * endpoint
) {
    assert(endpoint != NULL);
    return pomelo_atomic_int64_load(&endpoint->clock.offset);
}


int pomelo_delivery_endpoint_on_alloc(
    pomelo_delivery_endpoint_t * endpoint,
    pomelo_delivery_context_t * context
) {
    assert(endpoint != NULL);
    assert(context != NULL);

    endpoint->context = context;

    // Create array of buses
    pomelo_array_options_t array_options = {
        .allocator = context->allocator,
        .element_size = sizeof(pomelo_delivery_bus_t *),
        .initial_capacity = POMELO_DELIVERY_ENDPOINT_DEFAULT_BUSES_CAPACITY
    };
    endpoint->buses = pomelo_array_create(&array_options);
    if (!endpoint->buses) return -1;

    return 0;
}


void pomelo_delivery_endpoint_on_free(pomelo_delivery_endpoint_t * endpoint) {
    assert(endpoint != NULL);
    endpoint->context = NULL;

    if (endpoint->buses) {
        pomelo_array_destroy(endpoint->buses);
        endpoint->buses = NULL;
    }
}


int pomelo_delivery_endpoint_init(
    pomelo_delivery_endpoint_t * endpoint,
    pomelo_delivery_endpoint_options_t * info
) {
    assert(endpoint != NULL);
    assert(info != NULL);

    if (!info->platform || !info->sequencer || !info->heartbeat) {
        return -1; // Invalid info
    }

    pomelo_delivery_context_t * context = info->context;
    endpoint->platform = info->platform;
    endpoint->sequencer = info->sequencer;
    endpoint->heartbeat = info->heartbeat;
    endpoint->flags = 0;
    endpoint->out_reliable_capacity = POMELO_DEFAULT_RELIABLE_CAPACITY;
    endpoint->in_reliable_capacity = POMELO_DEFAULT_RELIABLE_CAPACITY;

    // Initialize the stop task
    pomelo_sequencer_task_init(
        &endpoint->stop_task,
        (pomelo_sequencer_callback) pomelo_delivery_endpoint_stop_deferred,
        endpoint
    );

    // Initialize the destroy task
    pomelo_sequencer_task_init(
        &endpoint->destroy_task,
        (pomelo_sequencer_callback) pomelo_delivery_endpoint_destroy_deferred,
        endpoint
    );

    // Initialize the heartbeat task
    pomelo_sequencer_task_init(
        &endpoint->heartbeat_task,
        (pomelo_sequencer_callback) pomelo_delivery_endpoint_send_ping,
        endpoint
    );

    // Initialize the ready task
    pomelo_sequencer_task_init(
        &endpoint->ready_task,
        (pomelo_sequencer_callback) pomelo_delivery_endpoint_on_ready,
        endpoint
    );

    // Set time sync flag
    if (info->time_sync) {
        endpoint->flags |= POMELO_DELIVERY_ENDPOINT_FLAG_TIME_SYNC;
    }

    // Resize bus array
    int ret = pomelo_array_resize(endpoint->buses, info->nbuses);
    if (ret < 0) return ret; // Failed to resize the bus array
    pomelo_array_fill_zero(endpoint->buses); // Fill the bus array with zeros

    // Acquire buses
    size_t nbuses = info->nbuses;
    pomelo_pool_t * bus_pool = context->bus_pool;
    for (size_t i = 0; i < nbuses; i++) {
        pomelo_delivery_bus_info_t bus_info = {
            .endpoint = endpoint,
            .id = i + 1 // ID 0 is reserved for the system bus
        };
        pomelo_delivery_bus_t * bus = pomelo_pool_acquire(bus_pool, &bus_info);
        if (!bus) return -1; // Failed to acquire the bus

        pomelo_array_set(endpoint->buses, i, bus);
    }

    // Acquire and initialize the system bus
    pomelo_delivery_bus_info_t bus_info = {
        .endpoint = endpoint,
        .id = 0 // ID 0 is reserved for the system bus
    };
    endpoint->system_bus = pomelo_pool_acquire(bus_pool, &bus_info);
    if (!endpoint->system_bus) return -1; // Failed to acquire the system bus

    // Initialize the rtt
    pomelo_rtt_calculator_init(&endpoint->rtt);

    // Initialize the clock
    pomelo_delivery_clock_init(&endpoint->clock, endpoint->platform);

    return 0;
}


void pomelo_delivery_endpoint_cleanup(pomelo_delivery_endpoint_t * endpoint) {
    assert(endpoint != NULL);

    // Release all buses of the endpoint
    pomelo_pool_t * bus_pool = endpoint->context->bus_pool;
    size_t nbuses = endpoint->buses->size;
    for (size_t i = 0; i < nbuses; i++) {
        pomelo_delivery_bus_t * bus = NULL;
        pomelo_array_get(endpoint->buses, i, &bus);
        if (!bus) continue;
        pomelo_pool_release(bus_pool, bus);
    }
    pomelo_array_clear(endpoint->buses);

    // Release the system bus
    if (endpoint->system_bus) {
        pomelo_pool_release(bus_pool, endpoint->system_bus);
        endpoint->system_bus = NULL;
    }

    // Stop the heartbeat
    pomelo_delivery_heartbeat_unschedule(endpoint->heartbeat, endpoint);
}


int pomelo_delivery_endpoint_start(pomelo_delivery_endpoint_t * endpoint) {
    assert(endpoint != NULL);

    // Start all buses
    size_t nbuses = endpoint->buses->size;
    for (size_t i = 0; i < nbuses; i++) {
        pomelo_delivery_bus_t * bus = NULL;
        pomelo_array_get(endpoint->buses, i, &bus);
        assert(bus != NULL);
        pomelo_delivery_bus_start(bus);
    }

    // Schedule the ping timer
    int ret = pomelo_delivery_heartbeat_schedule(endpoint->heartbeat, endpoint);
    if (ret < 0) return ret; // Failed to schedule the ping timer

    return 0;
}


void pomelo_delivery_endpoint_stop(pomelo_delivery_endpoint_t * endpoint) {
    assert(endpoint != NULL);
    pomelo_sequencer_submit(endpoint->sequencer, &endpoint->stop_task);
    // => pomelo_delivery_endpoint_stop_deferred()
}


void pomelo_delivery_endpoint_send_ping(pomelo_delivery_endpoint_t * endpoint) {
    assert(endpoint != NULL);

    // Acquire new parcel
    pomelo_delivery_parcel_t * parcel =
        pomelo_delivery_context_acquire_parcel(endpoint->context);
    if (!parcel) return; // Failed to acquire the parcel

    // Process send ping
    pomelo_delivery_endpoint_send_ping_ex(endpoint, parcel);

    // Unref the parcel
    pomelo_delivery_parcel_unref(parcel);
}


void pomelo_delivery_endpoint_send_ping_ex(
    pomelo_delivery_endpoint_t * endpoint,
    pomelo_delivery_parcel_t * parcel
) {
    assert(endpoint != NULL);
    assert(parcel != NULL);

    // Build the ping parcel
    pomelo_delivery_writer_t writer;
    pomelo_delivery_writer_init(&writer, parcel);
    
    // Build ping opcode-meta byte
    uint64_t time = pomelo_platform_hrtime(endpoint->platform);
    pomelo_rtt_entry_t * entry =
        pomelo_rtt_calculator_next_entry(&endpoint->rtt, time);
    assert(entry != NULL);

    /**
     * Ping layout:
     *  opcode: 3 bits
     *  sequence_bytes - 1: 1 bit
     *  time_sync: 1 bit
     *  reliable_cap_exp: 3 bits
     *  sequence: 1 - 8 bytes
     */

    uint8_t meta_byte = POMELO_DELIVERY_OPCODE_PING << 5;
    uint64_t sequence_bytes =
        pomelo_payload_calc_packed_uint64_bytes(entry->sequence);
    assert(sequence_bytes <= 2);
    meta_byte |= (uint8_t) ((sequence_bytes - 1) << 4);
    meta_byte |= (uint8_t)
        ((endpoint->flags & POMELO_DELIVERY_ENDPOINT_FLAG_TIME_SYNC) << 3);

    // Encode reliable capacity (3 bits: 0-7 mapping to 8,16,32,64,128,256,512,1024)
    size_t capacity = endpoint->in_reliable_capacity;
    assert(
        capacity >= POMELO_MIN_RELIABLE_CAPACITY &&
        capacity <= POMELO_MAX_RELIABLE_CAPACITY
    );

    // Calculate the exponent (3-10) and map to 0-7
    size_t exp = 64 - pomelo_clz_64(capacity - 1);
    size_t encoded = (exp < 3) ? 0 : ((exp > 10) ? 7 : (exp - 3));
    meta_byte |= (uint8_t) encoded;

    // Write the meta byte
    int ret = pomelo_delivery_writer_write(&writer, &meta_byte, 1);
    if (ret < 0) return; // Failed to write the meta byte

    // Write the sequence bytes
    uint8_t buffer[8];
    pomelo_payload_t payload = {
        .data = buffer,
        .capacity = sizeof(buffer),
        .position = 0
    };
    pomelo_payload_write_packed_uint64_unsafe(
        &payload,
        sequence_bytes,
        entry->sequence
    );
    ret = pomelo_delivery_writer_write(&writer, buffer, payload.position);
    if (ret < 0) return; // Failed to write the sequence

    // Prepare new sender
    pomelo_delivery_sender_options_t options = {
        .context = endpoint->context,
        .platform = endpoint->platform,
        .parcel = parcel
    };
    pomelo_delivery_sender_t * sender = pomelo_delivery_sender_create(&options);
    if (!sender) return; // Failed to create the sender

    // Set this sender as system sender
    sender->flags |= POMELO_DELIVERY_SENDER_FLAG_SYSTEM;

    // Add the recipient
    ret = pomelo_delivery_sender_add_transmission(
        sender,
        endpoint->system_bus,
        POMELO_DELIVERY_MODE_UNRELIABLE
    );
    if (ret < 0) {
        pomelo_delivery_sender_cancel(sender);
        return; // Failed to add the recipient
    }

    // Submit the sender
    pomelo_delivery_sender_submit(sender);
}


void pomelo_delivery_endpoint_send_pong(
    pomelo_delivery_endpoint_t * endpoint,
    uint64_t sequence,
    bool time_sync
) {
    assert(endpoint != NULL);

    // Acquire new parcel
    pomelo_delivery_parcel_t * parcel =
        pomelo_delivery_context_acquire_parcel(endpoint->context);
    if (!parcel) return; // Failed to acquire the parcel

    // Process send pong
    pomelo_delivery_endpoint_send_pong_ex(
        endpoint,
        sequence,
        time_sync,
        parcel
    );

    // Unref the parcel
    pomelo_delivery_parcel_unref(parcel);
}


void pomelo_delivery_endpoint_send_pong_ex(
    pomelo_delivery_endpoint_t * endpoint,
    uint64_t sequence,
    bool time_sync,
    pomelo_delivery_parcel_t * parcel
) {
    assert(endpoint != NULL);
    assert(parcel != NULL);
    /**
     * Pong layout:
     *  opcode: 3 bits
     *  sequence_bytes - 1: 1 bit
     *  time_sync: 1 bit
     *  time_bytes - 1: 3 bits
     *  sequence: 1 - 8 bytes
     *  time: 1 - 8 bytes
     */

    // Build the pong parcel
    pomelo_delivery_writer_t writer;
    pomelo_delivery_writer_init(&writer, parcel);

    // Build pong opcode-meta byte
    uint8_t meta_byte = POMELO_DELIVERY_OPCODE_PONG << 5;
    uint64_t sequence_bytes =
        pomelo_payload_calc_packed_uint64_bytes(sequence);
    assert(sequence_bytes <= 2);
    meta_byte |= (uint8_t) ((sequence_bytes - 1) << 4);
    meta_byte |= (uint8_t) (time_sync << 3);

    uint64_t time = 0;
    uint64_t time_bytes = 0;
    if (time_sync) {
        time = pomelo_platform_hrtime(endpoint->platform);
        time_bytes = pomelo_payload_calc_packed_uint64_bytes(time);
        meta_byte |= (uint8_t) (time_bytes - 1);
    }

    // Write the meta byte
    int ret = pomelo_delivery_writer_write(&writer, &meta_byte, 1);
    if (ret < 0) return; // Failed to write the meta byte

    // Write the sequence
    uint8_t buffer[8];
    pomelo_payload_t payload = {
        .data = buffer,
        .capacity = sizeof(buffer),
        .position = 0
    };
    pomelo_payload_write_packed_uint64_unsafe(
        &payload,
        sequence_bytes,
        sequence
    );
    ret = pomelo_delivery_writer_write(&writer, buffer, payload.position);
    if (ret < 0) return; // Failed to write the sequence

    // Write the time (If time sync is enabled)
    if (time_sync) {
        payload.position = 0; // Reset the payload position for writing
        pomelo_payload_write_packed_uint64_unsafe(&payload, time_bytes, time);
        ret = pomelo_delivery_writer_write(&writer, buffer, payload.position);
        if (ret < 0) return; // Failed to write the time
    }

    // Prepare new sender
    pomelo_delivery_sender_options_t options = {
        .context = endpoint->context,
        .platform = endpoint->platform,
        .parcel = parcel
    };
    pomelo_delivery_sender_t * sender = pomelo_delivery_sender_create(&options);
    if (!sender) return; // Failed to create the sender

    // Set this sender as system sender
    sender->flags |= POMELO_DELIVERY_SENDER_FLAG_SYSTEM;

    // Add the recipient
    ret = pomelo_delivery_sender_add_transmission(
        sender,
        endpoint->system_bus,
        POMELO_DELIVERY_MODE_UNRELIABLE
    );
    if (ret < 0) {
        pomelo_delivery_sender_cancel(sender);
        return; // Failed to add the recipient
    }

    // Submit the sender
    pomelo_delivery_sender_submit(sender);
}


void pomelo_delivery_endpoint_recv_system_parcel(
    pomelo_delivery_endpoint_t * endpoint,
    pomelo_delivery_parcel_t * parcel
) {
    assert(endpoint != NULL);
    assert(parcel != NULL);

    pomelo_delivery_reader_t reader;
    pomelo_delivery_reader_init(&reader, parcel);

    // Read the meta byte
    uint8_t meta_byte = 0;
    int ret = pomelo_delivery_reader_read(&reader, &meta_byte, 1);
    if (ret < 0) return; // Failed to read the meta byte

    uint8_t opcode = meta_byte >> 5;
    switch (opcode) {
        case POMELO_DELIVERY_OPCODE_PING:
            pomelo_delivery_endpoint_recv_ping(endpoint, meta_byte, &reader);
            break;
        
        case POMELO_DELIVERY_OPCODE_PONG:
            pomelo_delivery_endpoint_recv_pong(endpoint, meta_byte, &reader);
            break;

        default:
            break;
    }
}


void pomelo_delivery_endpoint_recv_ping(
    pomelo_delivery_endpoint_t * endpoint,
    uint8_t meta_byte,
    pomelo_delivery_reader_t * reader
) {
    assert(endpoint != NULL);
    (void) reader;

    // Extract the sequence bytes and time sync flag from the meta byte
    size_t sequence_bytes = ((meta_byte >> 4) & 0x01) + 1;
    bool time_sync = (meta_byte >> 3) & 0x01;

    // Read the sequence bytes
    uint8_t buffer[8];
    int ret = pomelo_delivery_reader_read(reader, buffer, sequence_bytes);
    if (ret < 0) return; // Failed to read the sequence bytes

    // Wrap the buffer into a payload
    pomelo_payload_t payload = {
        .data = buffer,
        .capacity = sequence_bytes,
        .position = 0
    };

    // Read the sequence
    uint64_t sequence = 0;
    pomelo_payload_read_packed_uint64_unsafe(
        &payload,
        sequence_bytes,
        &sequence
    );

    // Send the pong
    pomelo_delivery_endpoint_send_pong(endpoint, sequence, time_sync);

    // Check ready
    if (!(endpoint->flags & POMELO_DELIVERY_ENDPOINT_FLAG_READY)) {
        size_t encoded = meta_byte & 0x07;
        // 2 ^ (3 + encoded)
        endpoint->out_reliable_capacity = (1 << (encoded + 3));
        
        // Ensure the decoded capacity doesn't exceed the maximum
        if (endpoint->out_reliable_capacity > POMELO_MAX_RELIABLE_CAPACITY) {
            endpoint->out_reliable_capacity = POMELO_MAX_RELIABLE_CAPACITY;
        }
        
        endpoint->flags |= POMELO_DELIVERY_ENDPOINT_FLAG_READY;
        pomelo_sequencer_submit(endpoint->sequencer, &endpoint->ready_task);
    }
}


void pomelo_delivery_endpoint_recv_pong(
    pomelo_delivery_endpoint_t * endpoint,
    uint8_t meta_byte,
    pomelo_delivery_reader_t * reader
) {
    assert(endpoint != NULL);
    assert(reader != NULL);

    // Extract the time sync flag
    bool time_sync = (meta_byte >> 3) & 0x01;
    if (!time_sync) return; // No time sync

    // Extract the sequence bytes
    size_t sequence_bytes = ((meta_byte >> 4) & 0x01) + 1;
    size_t time_bytes = meta_byte & 0x07;

    uint8_t buffer[8];

    // Read the sequence
    int ret = pomelo_delivery_reader_read(reader, buffer, sequence_bytes);
    if (ret < 0) return; // Failed to read the sequence bytes
    pomelo_payload_t payload = {
        .data = buffer,
        .capacity = sequence_bytes,
        .position = 0
    };
    
    uint64_t sequence = 0;
    pomelo_payload_read_packed_uint64_unsafe(
        &payload,
        sequence_bytes,
        &sequence
    );

    // Read the time
    ret = pomelo_delivery_reader_read(reader, buffer, time_bytes);
    if (ret < 0) return; // Failed to read the time bytes
    payload.position = 0;
    payload.capacity = time_bytes;

    uint64_t time = 0;
    pomelo_payload_read_packed_uint64_unsafe(
        &payload,
        time_bytes,
        &time
    );

    // Update the RTT 
    pomelo_rtt_entry_t * entry = pomelo_rtt_calculator_entry(
        &endpoint->rtt,
        sequence
    );
    if (!entry) return; // Failed to get the entry

    uint64_t recv_time = pomelo_platform_hrtime(endpoint->platform);
    pomelo_rtt_calculator_submit_entry(
        &endpoint->rtt,
        entry,
        recv_time,
        0 // Currently, req_recv_time and res_send_time are the same
    );

    // Update the clock
    if (endpoint->flags & POMELO_DELIVERY_ENDPOINT_FLAG_TIME_SYNC) {
        pomelo_delivery_clock_sync(
            &endpoint->clock,
            &endpoint->rtt,
            entry->time, // req_send_time
            time,        // req_recv_time
            time,        // res_send_time
            recv_time    // res_recv_time
        );
    }
}


void pomelo_delivery_endpoint_heartbeat(pomelo_delivery_endpoint_t * endpoint) {
    assert(endpoint != NULL);
    pomelo_sequencer_submit(endpoint->sequencer, &endpoint->heartbeat_task);
    // => pomelo_delivery_endpoint_send_ping()
}


void pomelo_delivery_endpoint_stop_deferred(pomelo_delivery_endpoint_t * endpoint) {
    assert(endpoint != NULL);

    // Stop all user buses
    pomelo_delivery_bus_t * bus;
    pomelo_array_t * buses = endpoint->buses;
    size_t nbuses = buses->size;
    for (size_t i = 0; i < nbuses; i++) {
        pomelo_array_get(buses, i, &bus);
        pomelo_delivery_bus_stop(bus);
    }

    // Stop the system bus
    pomelo_delivery_bus_stop(endpoint->system_bus);

    // Stop the heartbeat
    pomelo_delivery_heartbeat_unschedule(endpoint->heartbeat, endpoint);
}


void pomelo_delivery_endpoint_destroy_deferred(
    pomelo_delivery_endpoint_t * endpoint
) {
    assert(endpoint != NULL);
    pomelo_pool_release(endpoint->context->endpoint_pool, endpoint);
}

