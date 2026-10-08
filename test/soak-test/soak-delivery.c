#include "uv.h"
#include "pomelo-test.h"
#include "delivery/delivery.h"
#include "delivery/parcel.h"
#include "platform/uv/platform-uv.h"
#include "delivery/context.h"
#include "pomelo/random.h"
#include "base/constants.h"
#include "statistic-check/statistic-check.h"
#include "soak-test/soak-common.h"

#define SOAK_PAYLOAD_BYTES   3200
#define SOAK_NBUSES          3
// On main, delivery uses the single-dispatcher reliable model (at most one
// reliable parcel in flight per bus — head-of-line blocking). Keep one parcel
// in flight at a time so the reliable mode is never overlapped (which would
// trip bus.c:416 incomplete_reliable_receiver), while still churning the pool,
// fragment reassembly and retransmit path continuously over the run.
#define SOAK_TARGET_INFLIGHT 1

// Environment (cloned from delivery-test-single.c)
static uv_loop_t uv_loop;
static pomelo_allocator_t * allocator;
static pomelo_platform_t * platform;
static pomelo_sequencer_t sequencer;
static pomelo_delivery_endpoint_t * sender;
static pomelo_delivery_endpoint_t * receiver;
static pomelo_buffer_context_t * buffer_ctx;
static pomelo_delivery_context_t * delivery_ctx;
static pomelo_delivery_heartbeat_t * heartbeat;
static uint8_t data[SOAK_PAYLOAD_BYTES];

// Soak state
static pomelo_soak_t soak;
static size_t in_flight = 0;
static size_t ready_count = 0;
static size_t mode_index = 0;
static int endpoints_stopped = 0;

// Stop the endpoints exactly once, only after the in-flight parcel has drained,
// so no parcel is leaked by stopping mid-delivery.
static void try_finish_drain(void) {
    if (soak.stopped && in_flight == 0 && !endpoints_stopped) {
        endpoints_stopped = 1;
        pomelo_delivery_endpoint_stop(sender);
        pomelo_delivery_endpoint_stop(receiver);
    }
}

static pomelo_delivery_mode modes[] = {
    POMELO_DELIVERY_MODE_SEQUENCED,
    POMELO_DELIVERY_MODE_UNRELIABLE,
    POMELO_DELIVERY_MODE_RELIABLE
};

static void send_one_parcel(pomelo_delivery_mode mode) {
    pomelo_delivery_bus_t * bus = pomelo_delivery_endpoint_get_bus(sender, 1);
    pomelo_check(bus != NULL);

    pomelo_delivery_parcel_t * parcel =
        pomelo_delivery_context_acquire_parcel(delivery_ctx);
    pomelo_check(parcel != NULL);

    pomelo_delivery_writer_t writer;
    pomelo_delivery_writer_init(&writer, parcel);
    pomelo_delivery_writer_write(&writer, data, sizeof(data));

    pomelo_delivery_sender_options_t options = {
        .context = delivery_ctx,
        .parcel = parcel,
        .platform = platform
    };
    pomelo_delivery_sender_t * snd = pomelo_delivery_sender_create(&options);
    pomelo_check(snd != NULL);

    int ret = pomelo_delivery_sender_add_transmission(snd, bus, mode);
    pomelo_check(ret == 0);
    pomelo_delivery_sender_submit(snd);
}

static void pump(void) {
    while (!soak.stopped && in_flight < SOAK_TARGET_INFLIGHT) {
        send_one_parcel(modes[mode_index++ % (sizeof(modes)/sizeof(modes[0]))]);
        in_flight++;
    }
}

// Transporter double: combine views and deliver to the peer. This soak is
// LOSSLESS on purpose. Dropping packets here exercises the reliable retransmit
// path, which on main (the old single-dispatcher reliable model) intermittently
// trips assert `incomplete_reliable_receiver == NULL` at bus.c:416 when a
// delayed retransmit races a completed reliable receiver — a real pre-existing
// delivery issue, not a soak-harness fault. A lossy/retransmit soak is deferred
// until that path is fixed (it is being replaced by the reliable-parallel model
// on the dev branch). See test/soak-test/README.md.
int pomelo_delivery_endpoint_send(
    pomelo_delivery_endpoint_t * endpoint,
    pomelo_buffer_view_t * views,
    size_t nviews
) {
    pomelo_buffer_t * buffer = pomelo_buffer_context_acquire(buffer_ctx);
    if (!buffer) return -1;

    pomelo_buffer_view_t view;
    view.buffer = buffer;
    view.offset = 0;
    view.length = 0;
    for (size_t i = 0; i < nviews; i++) {
        pomelo_buffer_view_t * current = &views[i];
        pomelo_check(current->length > 0);
        memcpy(buffer->data + view.length,
               current->buffer->data + current->offset, current->length);
        view.length += current->length;
    }

    int ret = pomelo_delivery_endpoint_recv(
        (endpoint == sender) ? receiver : sender, &view);
    pomelo_check(ret == 0);
    pomelo_buffer_unref(buffer);
    return 0;
}

void pomelo_delivery_bus_on_received(
    pomelo_delivery_bus_t * bus,
    pomelo_delivery_parcel_t * parcel,
    pomelo_delivery_mode mode
) {
    (void) bus; (void) mode;
    pomelo_delivery_reader_t reader;
    pomelo_delivery_reader_init(&reader, parcel);
    size_t remain = pomelo_delivery_reader_remain_bytes(&reader);
    pomelo_check(remain == sizeof(data));
}

void pomelo_delivery_sender_on_result(
    pomelo_delivery_sender_t * delivery_sender,
    pomelo_delivery_parcel_t * parcel,
    size_t transmission_count
) {
    (void) delivery_sender; (void) transmission_count;
    pomelo_delivery_parcel_unref(parcel);
    if (in_flight > 0) in_flight--;
    pomelo_soak_tick(&soak);
    if (soak.stopped) {
        try_finish_drain();   // last parcel settled -> stop endpoints
    } else {
        pump();               // keep one parcel in flight
    }
}

void pomelo_delivery_endpoint_on_ready(pomelo_delivery_endpoint_t * endpoint) {
    (void) endpoint;
    ready_count++;
    if (ready_count == 2) pump();
}

// Sample callback: order MUST match the pomelo_soak_watch() calls in main().
static void soak_sample(void * ud, uint64_t * values, size_t n) {
    (void) ud; (void) n;
    pomelo_statistic_delivery_t d;
    pomelo_delivery_context_statistic(delivery_ctx, &d);
    pomelo_statistic_buffer_t b;
    pomelo_buffer_context_statistic(buffer_ctx, &b);

    values[0]  = d.parcels;
    values[1]  = d.dispatchers;
    values[2]  = d.senders;
    values[3]  = d.receivers;
    values[4]  = d.transmissions;
    values[5]  = d.receptions;
    values[6]  = d.endpoints;
    values[7]  = d.buses;
    values[8]  = d.heartbeats;
    values[9]  = b.buffers;
    values[10] = pomelo_allocator_allocated_bytes(allocator);
}

static void soak_stop(void * ud) {
    (void) ud;
    // If nothing is in flight, stop now; otherwise the pending result callback
    // will drain and stop (see try_finish_drain).
    try_finish_drain();
}

int main(int argc, char ** argv) {
    printf("Delivery soak\n");
    pomelo_random_buffer(data, SOAK_PAYLOAD_BYTES);

    allocator = pomelo_allocator_default();
    uint64_t alloc_bytes = pomelo_allocator_allocated_bytes(allocator);
    uv_loop_init(&uv_loop);

    pomelo_buffer_context_root_options_t buffer_ctx_options = {
        .allocator = allocator, .buffer_capacity = POMELO_BUFFER_CAPACITY
    };
    buffer_ctx = pomelo_buffer_context_root_create(&buffer_ctx_options);
    pomelo_check(buffer_ctx != NULL);

    pomelo_platform_uv_options_t platform_options = {
        .allocator = allocator, .uv_loop = &uv_loop
    };
    platform = pomelo_platform_uv_create(&platform_options);
    pomelo_check(platform != NULL);

    pomelo_delivery_context_root_options_t context_options = {
        .allocator = allocator, .buffer_context = buffer_ctx,
        .fragment_capacity = POMELO_PACKET_BODY_CAPACITY
    };
    delivery_ctx = pomelo_delivery_context_root_create(&context_options);
    pomelo_check(delivery_ctx != NULL);

    pomelo_delivery_heartbeat_options_t heartbeat_options = {
        .context = delivery_ctx, .platform = platform
    };
    heartbeat = pomelo_delivery_heartbeat_create(&heartbeat_options);
    pomelo_check(heartbeat != NULL);

    pomelo_sequencer_init(&sequencer);

    pomelo_delivery_endpoint_options_t options = {
        .context = delivery_ctx, .platform = platform, .heartbeat = heartbeat,
        .sequencer = &sequencer, .nbuses = SOAK_NBUSES
    };
    options.time_sync = false;
    sender = pomelo_delivery_endpoint_create(&options);
    pomelo_check(sender != NULL);
    options.time_sync = true;
    receiver = pomelo_delivery_endpoint_create(&options);
    pomelo_check(receiver != NULL);

    // Soak harness setup
    memset(&soak, 0, sizeof(soak));
    soak.platform = platform;
    soak.sample = soak_sample;
    soak.stop = soak_stop;
    soak.progress_floor = 10;
    pomelo_soak_config_from_args(&soak, argc, argv);
    pomelo_soak_watch(&soak, "parcels",       16, 0);
    pomelo_soak_watch(&soak, "dispatchers",   16, 0);
    pomelo_soak_watch(&soak, "senders",       16, 0);
    pomelo_soak_watch(&soak, "receivers",     16, 0);
    pomelo_soak_watch(&soak, "transmissions", 16, 0);
    pomelo_soak_watch(&soak, "receptions",    16, 0);
    pomelo_soak_watch(&soak, "endpoints",      0, 0);
    pomelo_soak_watch(&soak, "buses",          0, 0);
    pomelo_soak_watch(&soak, "heartbeats",     4, 0);
    pomelo_soak_watch(&soak, "buffers",       32, 0);
    pomelo_soak_watch(&soak, "alloc_bytes",    0, 25);

    pomelo_delivery_endpoint_start(sender);
    pomelo_delivery_endpoint_start(receiver);
    pomelo_soak_start(&soak);

    uv_run(&uv_loop, UV_RUN_DEFAULT);
    uv_loop_close(&uv_loop);

    pomelo_soak_finish(&soak);

    pomelo_delivery_endpoint_destroy(sender);
    pomelo_delivery_endpoint_destroy(receiver);
    pomelo_delivery_heartbeat_destroy(heartbeat);

    pomelo_statistic_delivery_t sd;
    pomelo_delivery_context_statistic(delivery_ctx, &sd);
    pomelo_statistic_delivery_check_resource_leak(&sd);
    pomelo_statistic_buffer_t sb;
    pomelo_buffer_context_statistic(buffer_ctx, &sb);
    pomelo_statistic_buffer_check_resource_leak(&sb);

    pomelo_delivery_context_destroy(delivery_ctx);
    pomelo_platform_uv_destroy(platform);
    pomelo_buffer_context_destroy(buffer_ctx);
    pomelo_check(alloc_bytes == pomelo_allocator_allocated_bytes(allocator));
    return 0;
}
