#include "uv.h"
#include "pomelo-test.h"
#include "delivery/delivery.h"
#include "delivery/parcel.h"
#include "delivery/bus.h"
#include "delivery/fragment.h"
#include "platform/uv/platform-uv.h"
#include "delivery/context.h"
#include "pomelo/random.h"
#include "base/constants.h"
#include "statistic-check/statistic-check.h"


/**
 * Regression test for the delivery malformed-fragment guards.
 *
 * PRIMARY guard (src/delivery/bus.c): pomelo_delivery_bus_recv_fragment_data()
 * rejects a crafted fragment whose last_index >= context->max_fragments with a
 * synchronous `return -1` at the top of the function, before any receiver /
 * sequencer work. This prevents memory-amplification from a crafted oversized
 * fragment count.
 *
 * SECONDARY guard (src/delivery/receiver.c): pomelo_delivery_receiver_add_fragment()
 * discards a fragment whose fragment_index is out of range (array_get_ptr
 * returns NULL) without any out-of-bounds access. We reach this by crafting a
 * fragment with a valid last_index but fragment_index > last_index.
 *
 * The test drives the real recv path directly on a usable bus (obtained the
 * same way delivery-test-single.c does), asserts the guards reject/discard the
 * malformed input without crashing, and that no parcel is ever delivered.
 */


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

// State
static size_t ready_count = 0;
static volatile int tests_ran = 0;
static volatile int on_received_count = 0;
static pomelo_platform_handle_t deferred_timer;


/* -------------------------------------------------------------------------- */
/*            Weak callbacks required by the delivery library                 */
/* -------------------------------------------------------------------------- */

// Should never fire: no crafted malformed fragment may complete a parcel.
void pomelo_delivery_bus_on_received(
    pomelo_delivery_bus_t * bus,
    pomelo_delivery_parcel_t * parcel,
    pomelo_delivery_mode mode
) {
    (void) bus;
    (void) parcel;
    (void) mode;
    on_received_count++;
}


// Unused in this test (we never submit a sender), but must resolve the symbol.
void pomelo_delivery_sender_on_result(
    pomelo_delivery_sender_t * delivery_sender,
    pomelo_delivery_parcel_t * parcel,
    size_t transmission_count
) {
    (void) delivery_sender;
    (void) parcel;
    (void) transmission_count;
}


// Forward whole payloads to the peer endpoint. This loopback is required so the
// heartbeat ping/pong readiness handshake completes and on_ready fires for both
// endpoints (see pomelo_delivery_endpoint_recv_ping). We never craft malformed
// fragments through this path; the crafted metas are injected directly into the
// bus recv entry in run_malformed_tests().
int pomelo_delivery_endpoint_send(
    pomelo_delivery_endpoint_t * endpoint,
    pomelo_buffer_view_t * views,
    size_t nviews
) {
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

    int ret = pomelo_delivery_endpoint_recv(
        (endpoint == sender) ? receiver : sender,
        &view
    );
    pomelo_check(ret == 0);
    pomelo_buffer_unref(buffer);
    return 0;
}


/* -------------------------------------------------------------------------- */
/*                         The malformed-fragment tests                       */
/* -------------------------------------------------------------------------- */

/// @brief Craft a fragment meta + a real content view and feed it to the bus.
static int feed_fragment(
    pomelo_delivery_bus_t * bus,
    pomelo_delivery_fragment_type type,
    size_t fragment_index,
    size_t last_index,
    uint64_t sequence
) {
    // A real, valid content buffer (so the only malformation is in the meta).
    pomelo_buffer_t * buffer = pomelo_buffer_context_acquire(buffer_ctx);
    pomelo_check(buffer != NULL);

    pomelo_buffer_view_t content;
    content.buffer = buffer;
    content.offset = 0;
    content.length = 16;

    pomelo_delivery_fragment_meta_t meta;
    meta.type = type;
    meta.bus_id = bus->id;
    meta.fragment_index = fragment_index;
    meta.last_index = last_index;
    meta.sequence = sequence;

    int ret = pomelo_delivery_bus_recv_fragment_data(bus, &meta, &content);

    // attach_content (if it happened) took its own ref; drop our local ref so
    // both accepted and rejected paths are leak-free.
    pomelo_buffer_unref(buffer);
    return ret;
}


static void run_malformed_tests(void) {
    pomelo_track_function();

    pomelo_delivery_bus_t * bus = pomelo_delivery_endpoint_get_bus(sender, 1);
    pomelo_check(bus != NULL);

    size_t max_fragments = bus->context->max_fragments;
    printf("[i] max_fragments = %zu\n", max_fragments);
    pomelo_check(max_fragments > 1);

    /* PRIMARY: last_index == max_fragments must be rejected (boundary). */
    int ret = feed_fragment(
        bus, POMELO_FRAGMENT_TYPE_DATA_UNRELIABLE, 0, max_fragments, 1001);
    printf("[i] last_index == max_fragments -> ret = %d\n", ret);
    pomelo_check(ret == -1);

    /* PRIMARY: last_index far beyond the cap must be rejected. */
    ret = feed_fragment(
        bus, POMELO_FRAGMENT_TYPE_DATA_UNRELIABLE, 0, max_fragments + 5, 1002);
    printf("[i] last_index == max_fragments + 5 -> ret = %d\n", ret);
    pomelo_check(ret == -1);

    /* BOUNDARY: last_index == max_fragments - 1 is valid and accepted. This
       proves the cutoff is exactly at max_fragments (not one off). The parcel
       stays incomplete (only fragment 0 of max_fragments supplied). */
    ret = feed_fragment(
        bus, POMELO_FRAGMENT_TYPE_DATA_UNRELIABLE, 0, max_fragments - 1, 1003);
    printf("[i] last_index == max_fragments - 1 -> ret = %d\n", ret);
    pomelo_check(ret == 0);

    /* SECONDARY: valid last_index but fragment_index out of range. The bus
       cap guard passes; receiver_add_fragment must discard the fragment
       (array_get_ptr -> NULL) without OOB and without completing the parcel. */
    ret = feed_fragment(
        bus, POMELO_FRAGMENT_TYPE_DATA_UNRELIABLE, 10, 5, 1004);
    printf("[i] fragment_index(10) > last_index(5) -> ret = %d\n", ret);
    pomelo_check(ret == 0); // function returns 0 after discarding the fragment

    // No malformed fragment may ever complete / deliver a parcel.
    pomelo_check(on_received_count == 0);

    tests_ran = 1;
    printf("[i] Malformed-fragment guard tests passed\n");
}


/// @brief Deferred entry: runs the crafted-fragment tests and stops endpoints.
/// Scheduled from on_ready via a one-shot platform timer so it executes at the
/// top of a loop iteration, NOT reentrantly inside the ping/pong forwarding
/// path (stopping endpoints mid-handshake corrupts list iteration).
static void run_tests_entry(void * data) {
    (void) data;

    // Stop the one-shot timer so its handle no longer keeps the loop alive.
    pomelo_platform_timer_stop(platform, &deferred_timer);

    run_malformed_tests();

    pomelo_delivery_endpoint_stop(sender);
    pomelo_delivery_endpoint_stop(receiver);
}


void pomelo_delivery_endpoint_on_ready(
    pomelo_delivery_endpoint_t * endpoint
) {
    pomelo_track_function();
    (void) endpoint;

    ready_count++;
    if (ready_count < 2) {
        return;
    }

    // Both endpoints ready. Defer the crafted-fragment tests + shutdown onto a
    // one-shot timer so they do not run reentrantly inside the readiness
    // handshake callback.
    int ret = pomelo_platform_timer_start(
        platform,
        run_tests_entry,
        1,  // 1 ms one-shot
        0,  // no repeat
        NULL,
        &deferred_timer
    );
    pomelo_check(ret == 0);
}


int main(void) {
    printf("Delivery malformed-fragment test\n");

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

    // Both endpoints use time_sync=false so they become ready without any real
    // network exchange (endpoint_send is a drop-everything stub in this test).
    options.time_sync = false;
    sender = pomelo_delivery_endpoint_create(&options);
    pomelo_check(sender != NULL);

    receiver = pomelo_delivery_endpoint_create(&options);
    pomelo_check(receiver != NULL);

    // Start the endpoints
    pomelo_delivery_endpoint_start(sender);
    pomelo_delivery_endpoint_start(receiver);

    /* Start testing */

    uv_run(&uv_loop, UV_RUN_DEFAULT);
    uv_loop_close(&uv_loop);

    /* End testing */

    // The crafted-fragment assertions must have actually run.
    pomelo_check(tests_ran == 1);
    pomelo_check(on_received_count == 0);

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
