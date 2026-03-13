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
 * It sends a parcel from one endpoint to another and verifies that the parcel
 * is received correctly.
 */


#define POMELO_TEST_DELIVERY_BUFFER_LENGTH 3200
#define POMELO_TEST_DELIVERY_NBUSES 3


// Environment
static uv_loop_t uv_loop;
static pomelo_allocator_t * allocator;
static pomelo_platform_t * platform;
static pomelo_sequencer_t sequencer;

// Endpoints
static pomelo_delivery_endpoint_t * sender;
static pomelo_delivery_endpoint_t * receiver;

// Contexts
static pomelo_buffer_context_t * buffer_ctx;
static pomelo_delivery_context_t * delivery_ctx;
static pomelo_delivery_heartbeat_t * heartbeat;

// Data to send
static uint8_t data[POMELO_TEST_DELIVERY_BUFFER_LENGTH];


// Temp variables
static size_t received_reliable_parcels = 0;
static size_t total_reliable_parcels = 0;
static size_t total_transmission_count = 0;
static int drop_chance = 0;
static size_t ready_count = 0;


static pomelo_delivery_mode modes[] = {
    POMELO_DELIVERY_MODE_SEQUENCED,
    POMELO_DELIVERY_MODE_UNRELIABLE,
    POMELO_DELIVERY_MODE_RELIABLE,
    POMELO_DELIVERY_MODE_SEQUENCED,
    POMELO_DELIVERY_MODE_RELIABLE,
    POMELO_DELIVERY_MODE_UNRELIABLE
};


/// @brief Check if this test should finish
static void check_finish(void) {
    if (total_transmission_count < sizeof(modes) / sizeof(modes[0])) {
        return;
    }

    if (received_reliable_parcels < total_reliable_parcels) {
        return;
    }

    printf("[i] Stopping endpoints...\n");

    // Stop the endpoints
    pomelo_delivery_endpoint_stop(sender);
    pomelo_delivery_endpoint_stop(receiver);
}


void pomelo_delivery_bus_on_received(
    pomelo_delivery_bus_t * bus,
    pomelo_delivery_parcel_t * parcel,
    pomelo_delivery_mode mode
) {
    pomelo_track_function();
    (void) bus;

    // Receiver will receive this parcel
    printf("[i] Received parcel: %zu fragments\n", parcel->chunks->size);

    // Check the parcel data
    uint8_t byte;
    int i = 0;
    pomelo_delivery_reader_t reader;
    pomelo_delivery_reader_init(&reader, parcel);
    size_t remain_bytes = pomelo_delivery_reader_remain_bytes(&reader);
    printf("[i] Parcel size = %zu\n", remain_bytes);
    pomelo_check(remain_bytes == sizeof(data));

    while (pomelo_delivery_reader_read(&reader, &byte, 1) == 0) {
        pomelo_check(byte == data[i]);
        i++;
    }

    printf("[i] Parcel data is valid, bytes = %d\n", i);

    if (mode == POMELO_DELIVERY_MODE_RELIABLE) {
        received_reliable_parcels++;
        check_finish();
    }
}


void pomelo_delivery_sender_on_result(
    pomelo_delivery_sender_t * delivery_sender,
    pomelo_delivery_parcel_t * parcel,
    size_t transmission_count
) {
    pomelo_track_function();
    (void) delivery_sender;

    pomelo_delivery_reader_t reader;
    pomelo_delivery_reader_init(&reader, parcel);
    size_t remain_bytes = pomelo_delivery_reader_remain_bytes(&reader);
    printf("[i] Sent parcel size = %zu\n", remain_bytes);
    pomelo_check(remain_bytes == sizeof(data));
    total_transmission_count += transmission_count;

    uint8_t byte;
    int index = 0;
    while (pomelo_delivery_reader_read(&reader, &byte, 1) == 0) {
        pomelo_check(byte == data[index]);
        index++;
    }

    printf(
        "[i] Sent parcel data is valid, bytes = %d, total sent count = %zu\n",
        index,
        total_transmission_count
    );

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
    drop_chance++;
    if (drop_chance % 5 == 0) {
        printf("[i] Drop the packet!!!\n");
        return 0; // Drop the packet
    }

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

    int ret = pomelo_delivery_endpoint_recv(
        (endpoint == sender) ? receiver : sender,
        &view
    );
    pomelo_check(ret == 0);
    pomelo_buffer_unref(buffer);

    return 0;
}



/// Test sending a parcel with a given mode
static void send_parcel(pomelo_delivery_mode mode) {
    pomelo_track_function();
    // Get the first bus for sending
    pomelo_delivery_bus_t * bus = pomelo_delivery_endpoint_get_bus(sender, 1);
    pomelo_check(bus != NULL);

    pomelo_delivery_parcel_t * parcel =
        pomelo_delivery_context_acquire_parcel(delivery_ctx);
    pomelo_check(parcel != NULL);

    pomelo_delivery_writer_t writer;
    pomelo_delivery_writer_init(&writer, parcel);
    pomelo_delivery_writer_write(&writer, data, sizeof(data));

    // Deliver the parcel
    pomelo_delivery_sender_options_t options = {
        .context = delivery_ctx,
        .parcel = parcel,
        .platform = platform
    };
    pomelo_delivery_sender_t * sender = pomelo_delivery_sender_create(&options);
    pomelo_check(sender != NULL);

    int ret = pomelo_delivery_sender_add_transmission(sender, bus, mode);
    pomelo_check(ret == 0);

    pomelo_delivery_sender_submit(sender);
}



void pomelo_delivery_endpoint_on_ready(
    pomelo_delivery_endpoint_t * endpoint
) {
    pomelo_track_function();

    if (endpoint == sender) {
        printf("[i] Sender is ready\n");
    } else {
        printf("[i] Receiver is ready\n");
    }

    ready_count++;
    if (ready_count == 2) {
        // Start sending parcels
        for (size_t i = 0; i < sizeof(modes) / sizeof(modes[0]); i++) {
            send_parcel(modes[i]);
        }
    }
}


int main(void) {
    printf("Delivery single test\n");

    // Random data
    pomelo_random_buffer(data, POMELO_TEST_DELIVERY_BUFFER_LENGTH);

    // Check the number of received parcels
    total_reliable_parcels = 0;
    for (size_t i = 0; i < sizeof(modes) / sizeof(modes[0]); i++) {
        if (modes[i] == POMELO_DELIVERY_MODE_RELIABLE) {
            total_reliable_parcels++;
        }
    }

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

    // Create transport context
    pomelo_delivery_context_root_options_t context_options = {
        .allocator = allocator,
        .buffer_context = buffer_ctx,
        .fragment_capacity = POMELO_PACKET_BODY_CAPACITY
    };
    delivery_ctx = pomelo_delivery_context_root_create(&context_options);
    pomelo_check(delivery_ctx != NULL);

    // Create heartbeat
    pomelo_delivery_heartbeat_options_t heartbeat_options = {
        .context = delivery_ctx,
        .platform = platform
    };
    heartbeat = pomelo_delivery_heartbeat_create(&heartbeat_options);
    pomelo_check(heartbeat != NULL);

    // Initialize sequencer
    pomelo_sequencer_init(&sequencer);

    pomelo_delivery_endpoint_options_t options = {
        .context = delivery_ctx,
        .platform = platform,
        .heartbeat = heartbeat,
        .sequencer = &sequencer,
        .nbuses = POMELO_TEST_DELIVERY_NBUSES
    };

    options.time_sync = false;
    sender = pomelo_delivery_endpoint_create(&options);
    pomelo_check(sender != NULL);

    options.time_sync = true;
    receiver = pomelo_delivery_endpoint_create(&options);
    pomelo_check(receiver != NULL);

    // Start the endpoints
    pomelo_delivery_endpoint_start(sender);
    pomelo_delivery_endpoint_start(receiver);

    /* Start testing */

    uv_run(&uv_loop, UV_RUN_DEFAULT);
    uv_loop_close(&uv_loop);

    /* End testing */

    // Destroy endpoints
    pomelo_delivery_endpoint_destroy(sender);
    pomelo_delivery_endpoint_destroy(receiver);

    // Destroy the heartbeat
    pomelo_delivery_heartbeat_destroy(heartbeat);

    // Check resource leak
    pomelo_statistic_delivery_t statistic_delivery;
    pomelo_delivery_context_statistic(delivery_ctx, &statistic_delivery);
    pomelo_statistic_delivery_check_resource_leak(&statistic_delivery);

    pomelo_statistic_buffer_t statistic_buffer;
    pomelo_buffer_context_statistic(buffer_ctx, &statistic_buffer);
    pomelo_statistic_buffer_check_resource_leak(&statistic_buffer);

    // Destroy platform and contexts
    pomelo_delivery_context_destroy(delivery_ctx);
    pomelo_platform_uv_destroy(platform);
    pomelo_buffer_context_destroy(buffer_ctx);

    pomelo_check(alloc_bytes == pomelo_allocator_allocated_bytes(allocator));

    return 0;
}
