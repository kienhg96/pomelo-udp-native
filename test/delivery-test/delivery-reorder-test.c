#include "uv.h"
#include "pomelo-test.h"
#include "delivery/delivery.h"
#include "delivery/parcel.h"
#include "delivery/fragment.h"
#include "platform/uv/platform-uv.h"
#include "delivery/context.h"
#include "base/buffer.h"
#include "pomelo/random.h"
#include "base/constants.h"
#include "statistic-check/statistic-check.h"


/**
 * This test proves out-of-order fragment reassembly and duplicate-fragment
 * dedup within a SINGLE reliable parcel.
 *
 * Instead of forwarding each outgoing fragment immediately (like the single
 * test does), the transporter double queues every data-bus fragment the sender
 * emits. Once all fragments of the parcel are queued, they are flushed to the
 * receiver in REVERSE order (out of order) and one of them is delivered TWICE
 * (duplicate).
 *
 * Expectations:
 *  - The receiver reassembles the parcel correctly despite the out-of-order +
 *    duplicate delivery (dedup via receiver.c line ~284).
 *  - The parcel is dispatched exactly once (received_count == 1): the duplicate
 *    MUST NOT cause a second dispatch nor corrupt the reassembly.
 */


#define POMELO_TEST_DELIVERY_BUFFER_LENGTH 3200
#define POMELO_TEST_DELIVERY_NBUSES 3
#define POMELO_TEST_MAX_QUEUED 64
#define POMELO_TEST_WATCHDOG_MS 5000


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

// Watchdog (guards against the no-dedup failure mode hanging forever)
static uv_timer_t watchdog;

// Data to send
static uint8_t data[POMELO_TEST_DELIVERY_BUFFER_LENGTH];


// Completion flags / counters (asserted AFTER uv_run)
static volatile int received_count = 0;
static volatile int sender_done = 0;
static size_t ready_count = 0;

// Fragment queue for out-of-order + duplicate replay
static pomelo_buffer_t * queued_buffers[POMELO_TEST_MAX_QUEUED];
static size_t queued_lengths[POMELO_TEST_MAX_QUEUED];
static size_t queued_count = 0;
static size_t expected_fragments = 0;
static int flushed = 0;

// Shutdown bookkeeping
static int stopping = 0;
static int watchdog_closed = 0;


/// @brief Close the watchdog timer once (so uv_run can drain on success)
static void close_watchdog(void) {
    if (watchdog_closed) return;
    watchdog_closed = 1;
    uv_timer_stop(&watchdog);
    uv_close((uv_handle_t *) &watchdog, NULL);
}


/// @brief Stop both endpoints exactly once and close the watchdog
static void stop_all(void) {
    if (stopping) return;
    stopping = 1;
    printf("[i] Stopping endpoints...\n");
    pomelo_delivery_endpoint_stop(sender);
    pomelo_delivery_endpoint_stop(receiver);
    close_watchdog();
}


/// @brief Check if this test should finish (success path)
static void check_finish(void) {
    if (received_count < 1) return;
    if (sender_done < 1) return;
    stop_all();
}


/// @brief Watchdog: force the loop to drain so the final assertions can run
static void on_watchdog(uv_timer_t * timer) {
    (void) timer;
    printf("[!] Watchdog fired: forcing shutdown (received_count=%d)\n",
        received_count);
    // Do not go through close_watchdog() flag path twice; stop_all closes it.
    stop_all();
}


/// @brief Flush the queued fragments out of order + one duplicate
static void flush_fragments(void) {
    flushed = 1;

    printf(
        "[i] Flushing %zu fragments in REVERSE order (+1 duplicate)\n",
        queued_count
    );

    // Deliver in reverse arrival order (fragments were queued in index order).
    for (size_t k = queued_count; k > 0; k--) {
        size_t i = k - 1;

        pomelo_buffer_view_t view;
        view.buffer = queued_buffers[i];
        view.offset = 0;
        view.length = queued_lengths[i];

        int ret = pomelo_delivery_endpoint_recv(receiver, &view);
        pomelo_check(ret == 0);

        // Right after delivering the first (highest-index) fragment, deliver it
        // AGAIN as a duplicate while the parcel is still incomplete. The dedup
        // guard must drop it; otherwise recv_fragments is double-counted.
        if (i == queued_count - 1) {
            pomelo_buffer_view_t dup;
            dup.buffer = queued_buffers[i];
            dup.offset = 0;
            dup.length = queued_lengths[i];

            printf("[i] Delivering DUPLICATE fragment (queue slot %zu)\n", i);
            ret = pomelo_delivery_endpoint_recv(receiver, &dup);
            pomelo_check(ret == 0);
        }
    }

    // Release our queued references (receiver has taken its own refs)
    for (size_t i = 0; i < queued_count; i++) {
        pomelo_buffer_unref(queued_buffers[i]);
        queued_buffers[i] = NULL;
    }
    queued_count = 0;
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

    // Must be a genuinely multi-fragment parcel
    pomelo_check(parcel->chunks->size > 1);

    // Check the parcel data byte-by-byte
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
        received_count++;
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
    (void) transmission_count;

    pomelo_delivery_reader_t reader;
    pomelo_delivery_reader_init(&reader, parcel);
    size_t remain_bytes = pomelo_delivery_reader_remain_bytes(&reader);
    printf("[i] Sent parcel size = %zu\n", remain_bytes);
    pomelo_check(remain_bytes == sizeof(data));

    uint8_t byte;
    int index = 0;
    while (pomelo_delivery_reader_read(&reader, &byte, 1) == 0) {
        pomelo_check(byte == data[index]);
        index++;
    }

    printf("[i] Sent parcel data is valid, bytes = %d\n", index);

    pomelo_delivery_parcel_unref(parcel);
    sender_done++;
    check_finish();
}


int pomelo_delivery_endpoint_send(
    pomelo_delivery_endpoint_t * endpoint,
    pomelo_buffer_view_t * views,
    size_t nviews
) {
    pomelo_track_function();

    // Combine the views into a single buffer view (one on-wire packet)
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

    // Decode the fragment meta on a copy so we don't disturb 'view'.
    // bus_id == 0 means system traffic (ping/pong); bus_id != 0 is a data
    // fragment. We only reorder the sender's data fragments.
    pomelo_delivery_fragment_meta_t meta;
    pomelo_buffer_view_t meta_view = view;
    int decoded = pomelo_delivery_fragment_meta_decode(&meta, &meta_view);

    int is_sender_fragment =
        (endpoint == sender) && (decoded == 0) && (meta.bus_id != 0);

    if (!is_sender_fragment) {
        // System packets (ping/pong) and the receiver's ACKs flow immediately.
        int ret = pomelo_delivery_endpoint_recv(
            (endpoint == sender) ? receiver : sender,
            &view
        );
        pomelo_check(ret == 0);
        pomelo_buffer_unref(buffer);
        return 0;
    }

    // Sender data fragment.
    if (flushed) {
        // Fragments arriving after the flush are retransmissions: drop them so
        // the no-dedup failure mode cannot silently "recover".
        printf("[i] Dropping post-flush retransmitted fragment\n");
        pomelo_buffer_unref(buffer);
        return 0;
    }

    // Queue the fragment (keep the buffer alive until it is replayed).
    pomelo_check(queued_count < POMELO_TEST_MAX_QUEUED);
    queued_buffers[queued_count] = buffer; // keep our acquire ref
    queued_lengths[queued_count] = view.length;
    queued_count++;

    printf(
        "[i] Queued fragment index=%zu (bus=%zu), queued=%zu/%zu\n",
        meta.fragment_index,
        meta.bus_id,
        queued_count,
        expected_fragments
    );

    // Once every fragment of the parcel has been queued, replay them.
    if (expected_fragments > 0 && queued_count == expected_fragments) {
        flush_fragments();
    }

    return 0;
}


/// Send a single reliable parcel that spans multiple fragments
static void send_parcel(void) {
    pomelo_track_function();

    pomelo_delivery_bus_t * bus = pomelo_delivery_endpoint_get_bus(sender, 1);
    pomelo_check(bus != NULL);

    pomelo_delivery_parcel_t * parcel =
        pomelo_delivery_context_acquire_parcel(delivery_ctx);
    pomelo_check(parcel != NULL);

    pomelo_delivery_writer_t writer;
    pomelo_delivery_writer_init(&writer, parcel);
    pomelo_delivery_writer_write(&writer, data, sizeof(data));

    // The number of fragments this parcel will produce.
    expected_fragments = parcel->chunks->size;
    printf("[i] Parcel spans %zu fragments\n", expected_fragments);
    pomelo_check(expected_fragments > 1);

    pomelo_delivery_sender_options_t options = {
        .context = delivery_ctx,
        .parcel = parcel,
        .platform = platform
    };
    pomelo_delivery_sender_t * delivery_sender =
        pomelo_delivery_sender_create(&options);
    pomelo_check(delivery_sender != NULL);

    int ret = pomelo_delivery_sender_add_transmission(
        delivery_sender,
        bus,
        POMELO_DELIVERY_MODE_RELIABLE
    );
    pomelo_check(ret == 0);

    pomelo_delivery_sender_submit(delivery_sender);
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
        send_parcel();
    }
}


int main(void) {
    printf("Delivery reorder test\n");

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

    // Start the watchdog so a broken dedup cannot hang the loop forever
    uv_timer_init(&uv_loop, &watchdog);
    uv_timer_start(&watchdog, on_watchdog, POMELO_TEST_WATCHDOG_MS, 0);

    /* Start testing */

    uv_run(&uv_loop, UV_RUN_DEFAULT);
    uv_loop_close(&uv_loop);

    /* End testing */

    // Dedup proof: the duplicate fragment must NOT cause a second dispatch, and
    // the out-of-order delivery must still reassemble exactly one parcel.
    pomelo_check(received_count == 1);
    pomelo_check(sender_done == 1);
    pomelo_check(flushed == 1);

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
