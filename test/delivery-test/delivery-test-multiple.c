#include "uv.h"
#include "pomelo-test.h"
#include "delivery/delivery.h"
#include "delivery/parcel.h"
#include "platform/uv/platform-uv.h"
#include "delivery/context.h"
#include "pomelo/random.h"
#include "base/constants.h"
#include "statistic-check/statistic-check.h"


/**
 * This test is used to verify the basic functionality of the delivery system.
 * It sends a parcel from one endpoint to multiple endpoints and verifies that
 * the parcel is received correctly.
 */


#define POMELO_TEST_DELIVERY_BUFFER_LENGTH 1500
#define POMELO_TEST_DELIVERY_NENDPOINTS 3
#define POMELO_TEST_DELIVERY_NBUSES 2

// Environment
static uv_loop_t uv_loop;
static pomelo_allocator_t * allocator;
static pomelo_platform_t * platform;
static pomelo_sequencer_t sequencer;


// Endpoints
static pomelo_delivery_endpoint_t * senders[POMELO_TEST_DELIVERY_NENDPOINTS];
static pomelo_delivery_endpoint_t * receivers[POMELO_TEST_DELIVERY_NENDPOINTS];

// Contexts
static pomelo_buffer_context_t * buffer_ctx;
static pomelo_delivery_context_t * delivery_ctx;
static pomelo_delivery_heartbeat_t * heartbeat;

// Data to send
static uint8_t data[POMELO_TEST_DELIVERY_BUFFER_LENGTH];

static size_t total_reliable_parcels = 0;
static size_t received_reliable_parcels = 0;
static size_t total_transmission_count = 0;
static size_t ready_count = 0;


// Temp variables
static pomelo_delivery_mode modes[] = {
    POMELO_DELIVERY_MODE_SEQUENCED,
    POMELO_DELIVERY_MODE_UNRELIABLE,
    POMELO_DELIVERY_MODE_RELIABLE,
    POMELO_DELIVERY_MODE_SEQUENCED,
    POMELO_DELIVERY_MODE_RELIABLE,
    POMELO_DELIVERY_MODE_UNRELIABLE
};


static void check_finish(void) {
    if (total_transmission_count <
        POMELO_TEST_DELIVERY_NENDPOINTS * (sizeof(modes) / sizeof(modes[0]))
    ) return;

    if (received_reliable_parcels <
        POMELO_TEST_DELIVERY_NENDPOINTS * total_reliable_parcels
    ) return;

    printf("[i] Stopping endpoints...");

    for (int i = 0; i < POMELO_TEST_DELIVERY_NENDPOINTS; i++) {
        pomelo_delivery_endpoint_stop(senders[i]);
        pomelo_delivery_endpoint_stop(receivers[i]);
    }
}


void pomelo_delivery_bus_on_received(
    pomelo_delivery_bus_t * bus,
    pomelo_delivery_parcel_t * parcel,
    pomelo_delivery_mode mode
) {
    pomelo_track_function();
    (void) bus;
    (void) parcel;

    pomelo_delivery_reader_t reader;
    pomelo_delivery_reader_init(&reader, parcel);
    pomelo_check(reader.remain_bytes == POMELO_TEST_DELIVERY_BUFFER_LENGTH);
    for (size_t i = 0; i < POMELO_TEST_DELIVERY_BUFFER_LENGTH; i++) {
        uint8_t byte = 0;
        pomelo_delivery_reader_read(&reader, &byte, 1);
        pomelo_check(byte == data[i]);
    }

    if (mode == POMELO_DELIVERY_MODE_RELIABLE) {
        received_reliable_parcels++;
        check_finish();
    }
}


void pomelo_delivery_sender_on_result(
    pomelo_delivery_sender_t * sender,
    pomelo_delivery_parcel_t * parcel,
    size_t transmission_count
) {
    (void) sender;
    pomelo_track_function();
    pomelo_check(transmission_count == POMELO_TEST_DELIVERY_NENDPOINTS);
    pomelo_delivery_parcel_unref(parcel);

    total_transmission_count += transmission_count;
    check_finish();
}


int pomelo_delivery_endpoint_send(
    pomelo_delivery_endpoint_t * endpoint,
    pomelo_buffer_view_t * views,
    size_t nviews
) {
    pomelo_track_function();

    // Combine the views into a single view
    pomelo_buffer_t * buffer = pomelo_buffer_context_acquire(buffer_ctx);
    if (!buffer) return -1;

    pomelo_buffer_view_t view;
    view.buffer = buffer;
    view.offset = 0;
    view.length = 0;

    for (size_t i = 0; i < nviews; i++) {
        pomelo_buffer_view_t * current = &views[i];
        pomelo_check(current->length > 0);
        memcpy(
            buffer->data + view.length,
            current->buffer->data + current->offset,
            current->length
        );
        view.length += current->length;
    }

    printf(
        "[i] Transporter sends payload with %zu views, total length = %zu\n",
        nviews,
        view.length
    );

    pomelo_delivery_endpoint_t * receiver =
        pomelo_delivery_endpoint_get_extra(endpoint);
    pomelo_check(receiver != NULL);

    int ret = pomelo_delivery_endpoint_recv(receiver, &view);
    pomelo_check(ret == 0);

    pomelo_buffer_unref(buffer);
    return 0;
}


static void send_parcel(pomelo_delivery_mode mode) {
    printf("Sending parcel in mode %d\n", mode);
    // Get the first bus for sending
    pomelo_delivery_parcel_t * parcel =
        pomelo_delivery_context_acquire_parcel(delivery_ctx);
    pomelo_check(parcel != NULL);

    pomelo_delivery_writer_t writer;
    pomelo_delivery_writer_init(&writer, parcel);
    pomelo_delivery_writer_write(&writer, data, sizeof(data));

    pomelo_delivery_bus_t * buses[POMELO_TEST_DELIVERY_NENDPOINTS];
    for (int i = 0; i < POMELO_TEST_DELIVERY_NENDPOINTS; i++) {
        // Just get the second bus for sending
        buses[i] = pomelo_delivery_endpoint_get_bus(senders[i], 1);
        pomelo_check(buses[i] != NULL);
    }
    
    pomelo_delivery_sender_options_t options = {
        .context = delivery_ctx,
        .parcel = parcel,
        .platform = platform
    };
    pomelo_delivery_sender_t * sender = pomelo_delivery_sender_create(&options);
    pomelo_check(sender != NULL);

    for (size_t i = 0; i < POMELO_TEST_DELIVERY_NENDPOINTS; i++) {
        int ret = pomelo_delivery_sender_add_transmission(
            sender, buses[i], mode
        );
        pomelo_check(ret == 0);
    }

    pomelo_delivery_sender_submit(sender);
}


void pomelo_delivery_endpoint_on_ready(
    pomelo_delivery_endpoint_t * endpoint
) {
    (void) endpoint;
    pomelo_track_function();

    ready_count++;
    if (ready_count == POMELO_TEST_DELIVERY_NENDPOINTS * 2) {
        printf("[i] All endpoints are ready\n");
    
        // Start sending parcels
        for (size_t i = 0; i < sizeof(modes) / sizeof(modes[0]); i++) {
            send_parcel(modes[i]);
        }
    }
}


int main(void) {
    printf("Delivery multiple test\n");

    // Check the number of received parcels
    total_reliable_parcels = 0;
    for (size_t i = 0; i < sizeof(modes) / sizeof(modes[0]); i++) {
        if (modes[i] == POMELO_DELIVERY_MODE_RELIABLE) {
            total_reliable_parcels++;
        }
    }

    // Random data
    pomelo_random_buffer(data, POMELO_TEST_DELIVERY_BUFFER_LENGTH);

    allocator = pomelo_allocator_default();
    uint64_t alloc_bytes = pomelo_allocator_allocated_bytes(allocator);

    uv_loop_init(&uv_loop);

    // Create data context
    pomelo_buffer_context_root_options_t buffer_ctx_options = {
        .allocator = allocator,
        .buffer_capacity = POMELO_BUFFER_CAPACITY
    };
    buffer_ctx = pomelo_buffer_context_root_create(&buffer_ctx_options);
    pomelo_check(buffer_ctx != NULL);

    // Create the platform
    pomelo_platform_uv_options_t platform_options = {
        .allocator = allocator,
        .uv_loop = &uv_loop
    };
    platform = pomelo_platform_uv_create(&platform_options);
    pomelo_check(platform != NULL);

    // Create delivery context
    pomelo_delivery_context_root_options_t context_options = {
        .allocator = allocator,
        .buffer_context = buffer_ctx,
        .fragment_capacity = POMELO_PACKET_BODY_CAPACITY
    };
    delivery_ctx = pomelo_delivery_context_root_create(&context_options);
    pomelo_check(delivery_ctx != NULL);

    // Initialize sequencer
    pomelo_sequencer_init(&sequencer);

    // Create heartbeat
    pomelo_delivery_heartbeat_options_t heartbeat_options = {
        .context = delivery_ctx,
        .platform = platform
    };
    heartbeat = pomelo_delivery_heartbeat_create(&heartbeat_options);
    pomelo_check(heartbeat != NULL);

    // Create & start the endpoints
    for (int i = 0; i < POMELO_TEST_DELIVERY_NENDPOINTS; i++) {
        pomelo_delivery_endpoint_options_t options = {
            .context = delivery_ctx,
            .platform = platform,
            .sequencer = &sequencer,
            .heartbeat = heartbeat,
            .nbuses = POMELO_TEST_DELIVERY_NBUSES
        };
        senders[i] = pomelo_delivery_endpoint_create(&options);
        pomelo_check(senders[i] != NULL);

        receivers[i] = pomelo_delivery_endpoint_create(&options);
        pomelo_check(receivers[i] != NULL);

        pomelo_delivery_endpoint_set_extra(senders[i], receivers[i]);
        pomelo_delivery_endpoint_set_extra(receivers[i], senders[i]);

        pomelo_delivery_endpoint_start(senders[i]);
        pomelo_delivery_endpoint_start(receivers[i]);
    }

    /* Start testing */

    uv_run(&uv_loop, UV_RUN_DEFAULT);
    uv_loop_close(&uv_loop);

    /* End testing */

    // Destroy endpoints
    for (int i = 0; i < POMELO_TEST_DELIVERY_NENDPOINTS; i++) {
        pomelo_delivery_endpoint_destroy(senders[i]);
        pomelo_delivery_endpoint_destroy(receivers[i]);
    }

    // Destroy the heartbeat
    pomelo_delivery_heartbeat_destroy(heartbeat);

    // Check resource leak
    pomelo_statistic_delivery_t statistic;
    pomelo_delivery_context_statistic(delivery_ctx, &statistic);
    pomelo_statistic_delivery_check_resource_leak(&statistic);

    pomelo_statistic_buffer_t statistic_buffer;
    pomelo_buffer_context_statistic(buffer_ctx, &statistic_buffer);
    pomelo_statistic_buffer_check_resource_leak(&statistic_buffer);

    // Destroy platform and contexts    
    pomelo_delivery_context_destroy(delivery_ctx);
    pomelo_platform_uv_destroy(platform);
    pomelo_buffer_context_destroy(buffer_ctx);

    size_t expected = 0;
    for (size_t i = 0; i < sizeof(modes) / sizeof(modes[0]); i++) {
        if (modes[i] == POMELO_DELIVERY_MODE_RELIABLE) {
            expected++;
        }
    }
    expected *= POMELO_TEST_DELIVERY_NENDPOINTS;

    printf("Recv parcels = %zu / %zu\n", received_reliable_parcels, expected);
    pomelo_check(received_reliable_parcels >= expected);

    // Check memleak
    pomelo_check(alloc_bytes == pomelo_allocator_allocated_bytes(allocator));
    return 0;
}
