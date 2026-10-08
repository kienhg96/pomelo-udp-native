# Soak / Endurance Tests Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add four standalone, manually-run soak binaries (delivery, protocol, api, engine) that drive the engine under sustained load and assert a steady-state invariant (no occupancy gauge in `pomelo_statistic_t` climbs beyond baseline+slack after warmup) plus forward-progress and zero-leak.

**Architecture:** A header-only harness `test/soak-test/soak-common.h` owns the sampler (repeating 250 ms platform timer), the growth/progress guards, the stop-timer, and the progress reporter. Each driver is its own translation unit / executable that defines the layer's global callbacks, generates load, and provides a `sample` callback that copies the chosen occupancy counters. Binaries build only under `-DPOMELO_BUILD_SOAK=ON` and are **not** registered with CTest.

**Tech Stack:** C11, libuv, libsodium, CMake. Reuses `test/pomelo-test.h` (`pomelo_check`), `src/platform/platform.h` (timer API), the per-layer statistic getters, and `test/statistic-check`.

**Spec:** `docs/superpowers/specs/2026-10-08-soak-tests-design.md`

**Standing constraints:** local commits only — **DO NOT push**. Every commit ends with footer `Co-Authored-By: claude-flow <ruv@ruv.net>`.

---

## File structure (locked)

- Create `test/soak-test/soak-common.h` — shared harness (config, sampler, guards, stop-timer, reporter). One responsibility: measurement + lifecycle.
- Create `test/soak-test/soak-delivery.c` — delivery-layer load (clone setup from `test/delivery-test/delivery-test-single.c`).
- Create `test/soak-test/soak-protocol.c` — protocol-layer load (clone setup from `test/protocol-test/protocol-test.c`).
- Create `test/soak-test/soak-api.c` — API-layer load (clone setup from `test/api-test/api-basic-test.c`).
- Create `test/soak-test/soak-engine.c` — whole-engine combo (clone setup from `test/api-test/api-basic-test.c`, multiple sessions).
- Modify `CMakeLists.txt` — add `POMELO_BUILD_SOAK` option + four target blocks, inside the existing `if (POMELO_BUILD_TESTS)` region.

### Counter & slack tuning (applies to every driver)

A gauge that oscillates (e.g. `peers` during churn goes 0→N→0) has `high_water == N`; its `baseline` is a single post-warmup sample that may land mid-cycle. So **slack must cover the known concurrency amplitude**:
- Occupancy gauges with fixed small concurrency: `slack` = (max concurrent of that object) + small margin (default 4–16).
- `allocated_bytes`: use `pct = 25` (percentage allowance), not absolute slack.

The driver controls concurrency (fixed in-flight / fixed client pool), so every watched gauge is bounded and predictable.

---

## Task 1: Harness header + CMake opt-in scaffold

**Files:**
- Create: `test/soak-test/soak-common.h`
- Modify: `CMakeLists.txt` (add `option(POMELO_BUILD_SOAK ...)` near the other options, ~line 10; add an empty guarded soak section inside `if (POMELO_BUILD_TESTS)`, just before the closing `endif() # POMELO_BUILD_TESTS` at line 828)

- [ ] **Step 1: Write the harness header**

Create `test/soak-test/soak-common.h` with exactly this content:

```c
#ifndef POMELO_SOAK_COMMON_H
#define POMELO_SOAK_COMMON_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "pomelo-test.h"
#include "platform/platform.h"

#define POMELO_SOAK_MAX_COUNTERS 32
#define POMELO_SOAK_SAMPLE_MS    250

/* One watched occupancy gauge. */
typedef struct pomelo_soak_counter_s {
    const char * name;
    uint64_t     slack;       /* absolute allowance over baseline            */
    uint64_t     pct;         /* percent allowance; used instead of slack !=0 */
    uint64_t     baseline;    /* first post-warmup sample                     */
    uint64_t     high_water;  /* max seen after warmup                        */
    int          baseline_set;
} pomelo_soak_counter_t;

/* Driver fills values[0..ncounters-1] with the current gauge readings,
 * in the SAME order the counters were registered with pomelo_soak_watch(). */
typedef void (*pomelo_soak_sample_fn)(void * ud, uint64_t * values, size_t ncounters);

/* Driver stops its load generator + sockets/endpoints so the loop drains. */
typedef void (*pomelo_soak_stop_fn)(void * ud);

typedef struct pomelo_soak_s {
    pomelo_platform_t *      platform;
    uint64_t                 duration_ms;
    uint64_t                 warmup_ms;

    pomelo_soak_counter_t    counters[POMELO_SOAK_MAX_COUNTERS];
    size_t                   ncounters;

    pomelo_soak_sample_fn    sample;
    void *                   sample_ud;
    pomelo_soak_stop_fn      stop;
    void *                   stop_ud;

    uint64_t                 iterations;     /* driver bumps via pomelo_soak_tick() */
    uint64_t                 progress_floor;

    uint64_t                 elapsed_ms;     /* derived from sample ticks           */
    int                      stopped;
    pomelo_platform_handle_t sample_timer;
} pomelo_soak_t;

/* Resolve duration/warmup from env, with argv[1] overriding duration seconds.
 * POMELO_SOAK_SECONDS (default 30), POMELO_SOAK_WARMUP_SECONDS (default 3). */
static inline void pomelo_soak_config_from_args(
    pomelo_soak_t * soak, int argc, char ** argv
) {
    uint64_t seconds = 30;
    uint64_t warmup  = 3;

    const char * env_sec = getenv("POMELO_SOAK_SECONDS");
    if (env_sec && *env_sec) seconds = strtoull(env_sec, NULL, 10);

    const char * env_warm = getenv("POMELO_SOAK_WARMUP_SECONDS");
    if (env_warm && *env_warm) warmup = strtoull(env_warm, NULL, 10);

    if (argc > 1 && argv[1] && *argv[1]) seconds = strtoull(argv[1], NULL, 10);

    if (seconds == 0) seconds = 1;
    if (warmup >= seconds) warmup = 0;

    soak->duration_ms = seconds * 1000;
    soak->warmup_ms   = warmup * 1000;

    printf("[soak] duration=%llus warmup=%llus\n",
        (unsigned long long) seconds, (unsigned long long) warmup);
}

/* Register one watched gauge. Pass pct>0 for a percentage allowance
 * (e.g. allocated_bytes); otherwise slack is an absolute allowance. */
static inline void pomelo_soak_watch(
    pomelo_soak_t * soak, const char * name, uint64_t slack, uint64_t pct
) {
    pomelo_check(soak->ncounters < POMELO_SOAK_MAX_COUNTERS);
    pomelo_soak_counter_t * c = &soak->counters[soak->ncounters++];
    c->name = name;
    c->slack = slack;
    c->pct = pct;
    c->baseline = 0;
    c->high_water = 0;
    c->baseline_set = 0;
}

/* Driver calls this once per unit of work completed. */
static inline void pomelo_soak_tick(pomelo_soak_t * soak) {
    soak->iterations++;
}

/* Internal: the repeating sample timer entry. */
static void pomelo_soak_on_sample(void * data) {
    pomelo_soak_t * soak = (pomelo_soak_t *) data;
    soak->elapsed_ms += POMELO_SOAK_SAMPLE_MS;

    uint64_t values[POMELO_SOAK_MAX_COUNTERS];
    memset(values, 0, sizeof(values));
    soak->sample(soak->sample_ud, values, soak->ncounters);

    int warmed = (soak->elapsed_ms >= soak->warmup_ms);
    if (warmed) {
        for (size_t i = 0; i < soak->ncounters; i++) {
            pomelo_soak_counter_t * c = &soak->counters[i];
            uint64_t v = values[i];
            if (!c->baseline_set) {
                c->baseline = v;
                c->high_water = v;
                c->baseline_set = 1;
            } else if (v > c->high_water) {
                c->high_water = v;
            }
        }
    }

    printf("[soak] t=%llus iters=%llu",
        (unsigned long long)(soak->elapsed_ms / 1000),
        (unsigned long long) soak->iterations);
    for (size_t i = 0; i < soak->ncounters; i++) {
        printf(" %s=%llu", soak->counters[i].name,
            (unsigned long long) values[i]);
    }
    printf("\n");

    if (soak->elapsed_ms >= soak->duration_ms && !soak->stopped) {
        soak->stopped = 1;
        printf("[soak] duration reached, stopping load...\n");
        /* Stop the repeating timer from inside the loop so its handle closes. */
        pomelo_platform_timer_stop(soak->platform, &soak->sample_timer);
        soak->stop(soak->stop_ud);
    }
}

/* Arm the repeating sampler. Call right before uv_run. */
static inline void pomelo_soak_start(pomelo_soak_t * soak) {
    soak->elapsed_ms = 0;
    soak->stopped = 0;
    int ret = pomelo_platform_timer_start(
        soak->platform,
        pomelo_soak_on_sample,
        POMELO_SOAK_SAMPLE_MS,   /* first fire */
        POMELO_SOAK_SAMPLE_MS,   /* repeat     */
        soak,
        &soak->sample_timer
    );
    pomelo_check(ret == 0);
}

/* Evaluate guards after uv_run returns. Aborts (non-zero exit) on failure. */
static inline void pomelo_soak_finish(pomelo_soak_t * soak) {
    int ok = 1;

    printf("\n[soak] ==== summary ====\n");
    printf("[soak] iterations=%llu floor=%llu\n",
        (unsigned long long) soak->iterations,
        (unsigned long long) soak->progress_floor);

    if (soak->iterations < soak->progress_floor) {
        printf("[soak] FAIL progress: %llu < floor %llu\n",
            (unsigned long long) soak->iterations,
            (unsigned long long) soak->progress_floor);
        ok = 0;
    }

    for (size_t i = 0; i < soak->ncounters; i++) {
        pomelo_soak_counter_t * c = &soak->counters[i];
        uint64_t limit = c->baseline +
            (c->pct ? (c->baseline * c->pct / 100) : c->slack);
        int counter_ok = (c->high_water <= limit);
        printf("[soak] %-16s baseline=%llu high=%llu limit=%llu %s\n",
            c->name,
            (unsigned long long) c->baseline,
            (unsigned long long) c->high_water,
            (unsigned long long) limit,
            counter_ok ? "ok" : "GROWTH");
        if (!counter_ok) ok = 0;
    }

    printf("[soak] ==== %s ====\n\n", ok ? "PASS" : "FAIL");
    pomelo_check(ok);
}

#endif /* POMELO_SOAK_COMMON_H */
```

- [ ] **Step 2: Add the CMake option**

In `CMakeLists.txt`, after `option(POMELO_BUILD_GENERATOR "Build generator" ON)` (line 10), add:

```cmake
option(POMELO_BUILD_SOAK "Build standalone soak/endurance tests (manual, not CTest)" OFF)
```

- [ ] **Step 3: Add the guarded soak section skeleton**

In `CMakeLists.txt`, immediately before `endif() # POMELO_BUILD_TESTS` (line 828), add:

```cmake
    # Soak / endurance tests — standalone binaries, NOT registered with CTest.
    # Build with -DPOMELO_BUILD_SOAK=ON and run manually.
    if (POMELO_BUILD_SOAK)
        set(POMELO_SOAK_DELIVERY pomelo-soak-delivery)
        set(POMELO_SOAK_PROTOCOL pomelo-soak-protocol)
        set(POMELO_SOAK_API      pomelo-soak-api)
        set(POMELO_SOAK_ENGINE   pomelo-soak-engine)
        # Target blocks are appended by Tasks 2-5.
    endif() # POMELO_BUILD_SOAK
```

- [ ] **Step 4: Verify it configures**

Run: `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DPOMELO_BUILD_SOAK=ON`
Expected: configures with no error (the empty soak section is valid; no targets yet).

- [ ] **Step 5: Commit**

```bash
git add test/soak-test/soak-common.h CMakeLists.txt
git commit -m "test(soak): add soak harness header + POMELO_BUILD_SOAK option

Co-Authored-By: claude-flow <ruv@ruv.net>"
```

---

## Task 2: `soak-delivery` — delivery-layer steady state

**Files:**
- Create: `test/soak-test/soak-delivery.c`
- Modify: `CMakeLists.txt` (inside the `if (POMELO_BUILD_SOAK)` block from Task 1)

Clone the context/platform/endpoint **setup and teardown** verbatim from `test/delivery-test/delivery-test-single.c` (`main`, lines 248-349: allocator, `uv_loop`, buffer ctx, platform, delivery ctx, heartbeat, sequencer, two endpoints, start). Replace the fixed `modes[]`-driven completion with the continuous pump below, and delete the per-packet `printf` lines in the transporter/callbacks (a long run must stay quiet).

- [ ] **Step 1: Write the driver**

Create `test/soak-test/soak-delivery.c`:

```c
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
#define SOAK_TARGET_INFLIGHT 4

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
static int drop_chance = 0;

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

// Transporter double (combine views -> deliver to the peer). Drops 1/7 to
// exercise retransmit, like delivery-test-single.c but quiet.
int pomelo_delivery_endpoint_send(
    pomelo_delivery_endpoint_t * endpoint,
    pomelo_buffer_view_t * views,
    size_t nviews
) {
    drop_chance++;
    if (drop_chance % 7 == 0) return 0; // drop

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
    pump();
}

void pomelo_delivery_endpoint_on_ready(pomelo_delivery_endpoint_t * endpoint) {
    (void) endpoint;
    ready_count++;
    if (ready_count == 2) pump();
}

// Sample callback: order must match the pomelo_soak_watch() calls in main().
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
    pomelo_delivery_endpoint_stop(sender);
    pomelo_delivery_endpoint_stop(receiver);
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
```

- [ ] **Step 2: Add the CMake target**

Inside the `if (POMELO_BUILD_SOAK)` block in `CMakeLists.txt` (after the `set(...)` lines from Task 1), add:

```cmake
        add_executable(${POMELO_SOAK_DELIVERY} test/soak-test/soak-delivery.c)
        target_include_directories(${POMELO_SOAK_DELIVERY} PRIVATE ${POMELO_TEST_INCLUDE})
        target_link_libraries(${POMELO_SOAK_DELIVERY} PRIVATE
            ${POMELO_BASE}
            ${POMELO_UTILS}
            ${POMELO_CRYPTO}
            ${POMELO_DELIVERY}
            ${POMELO_PLATFORM_UV}
            ${POMELO_TEST_STATISTIC_CHECK}
            ${LIB_UV}
            ${LIB_SODIUM}
        )
        target_compile_options(${POMELO_SOAK_DELIVERY} PRIVATE ${POMELO_COMPILE_FLAGS})
```

- [ ] **Step 3: Build**

Run: `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DPOMELO_BUILD_SOAK=ON && cmake --build build --target pomelo-soak-delivery -j`
Expected: builds clean (no warnings-as-errors).

- [ ] **Step 4: Run a short soak — expect PASS**

Run: `POMELO_SOAK_SECONDS=5 ./build/pomelo-soak-delivery`
Expected: periodic `[soak] t=...` lines, then `[soak] ==== PASS ====`, exit code 0. Confirm `iterations` is well above the floor of 10.

- [ ] **Step 5: Soak revert-proof — prove the growth guard fires**

Temporarily inject a deliberate leak: in `pomelo_delivery_sender_on_result`, comment out `pomelo_delivery_parcel_unref(parcel);`. Rebuild and run `POMELO_SOAK_SECONDS=5 ./build/pomelo-soak-delivery`.
Expected: `parcels` (and `alloc_bytes`) high-water climbs past its limit → `[soak] ==== FAIL ====`, non-zero exit. This proves the guard is live.
Then **restore** the `pomelo_delivery_parcel_unref(parcel);` line exactly. Rebuild + run → PASS again. Confirm `git diff test/soak-test/soak-delivery.c` shows no residue.

- [ ] **Step 6: Commit**

```bash
git add test/soak-test/soak-delivery.c CMakeLists.txt
git commit -m "test(soak): delivery-layer steady-state soak

Co-Authored-By: claude-flow <ruv@ruv.net>"
```

---

## Task 3: `soak-protocol` — protocol packet-path steady state

**Files:**
- Create: `test/soak-test/soak-protocol.c`
- Modify: `CMakeLists.txt` (inside `if (POMELO_BUILD_SOAK)`)

Clone the full setup/teardown from `test/protocol-test/protocol-test.c` (`main`, ~lines 110-245: allocator, uv_loop, buffer ctx, platform, protocol ctx, two adapters, token encode, client+server create, start) and the global callbacks it defines (`pomelo_protocol_socket_on_connected/on_disconnected/on_received/on_connect_result`). Keep **one persistent client↔server connection** and sustain bidirectional payload traffic for the whole duration. (Connection *churn* — repeated connect/disconnect — is exercised by the API and engine soaks, Tasks 4–5, where recreating a client socket is the natural surface. The protocol soak stays focused on the per-packet path, which is where protocol-layer leaks live.)

- [ ] **Step 1: Write the driver**

Create `test/soak-test/soak-protocol.c`. Reuse the cloned setup; the soak-specific additions are:

```c
// --- add near the top (after the cloned includes) ---
#include "soak-test/soak-common.h"

#define SOAK_PROTO_PAYLOAD 256
#define SOAK_PROTO_INFLIGHT 4

static pomelo_soak_t soak;
static pomelo_protocol_peer_t * server_peer = NULL; // captured on server connect
static pomelo_protocol_peer_t * client_peer = NULL; // captured on client connect
static size_t proto_in_flight = 0;
static uint8_t proto_payload[SOAK_PROTO_PAYLOAD];

// Send one PAYLOAD from client_peer to the server.
static void proto_send_one(void) {
    if (soak.stopped || !client_peer) return;
    pomelo_buffer_view_t view;              // build a view over proto_payload
    // (use the same acquire/write/send idiom protocol-test.c uses in its
    //  on_connected handler, lines ~264-290: acquire a buffer from buffer_ctx,
    //  memcpy proto_payload in, set view.offset/length, then:)
    int ret = pomelo_protocol_peer_send(client_peer, &view, 1);
    pomelo_check(ret == 0);
    proto_in_flight++;
}

static void proto_pump(void) {
    while (!soak.stopped && client_peer && proto_in_flight < SOAK_PROTO_INFLIGHT)
        proto_send_one();
}
```

Wire the callbacks (adapt the cloned ones):
- `pomelo_protocol_socket_on_connected(socket, peer)`: if `socket == client` capture `client_peer = peer` and call `proto_pump()`; if `socket == server` capture `server_peer = peer`.
- `pomelo_protocol_socket_on_received(socket, peer, view)`: when the server receives, echo the payload back with `pomelo_protocol_peer_send(peer, &view, 1)`; when the client receives the echo, `proto_in_flight--`, `pomelo_soak_tick(&soak)`, `proto_pump()`.
- `pomelo_protocol_socket_on_disconnected` / `on_connect_result`: keep the cloned bodies but do **not** stop sockets on disconnect (the stop is driven by the soak timer).

Sample + stop + main:

```c
static void soak_sample(void * ud, uint64_t * values, size_t n) {
    (void) ud; (void) n;
    pomelo_statistic_protocol_t p;
    pomelo_protocol_context_statistic(protocol_ctx, &p);
    pomelo_statistic_buffer_t b;
    pomelo_buffer_context_statistic(buffer_ctx, &b);
    values[0] = p.peers;
    values[1] = p.senders;
    values[2] = p.receivers;
    values[3] = p.packets;
    values[4] = p.crypto_contexts;
    values[5] = b.buffers;
}

static void soak_stop(void * ud) {
    (void) ud;
    pomelo_protocol_socket_stop(client);
    pomelo_protocol_socket_stop(server);
}

// in main(), after sockets are started and before uv_run:
memset(&soak, 0, sizeof(soak));
soak.platform = platform;
soak.sample = soak_sample;
soak.stop = soak_stop;
soak.progress_floor = 10;
pomelo_random_buffer(proto_payload, SOAK_PROTO_PAYLOAD);
pomelo_soak_config_from_args(&soak, argc, argv);
pomelo_soak_watch(&soak, "peers",           4,  0);
pomelo_soak_watch(&soak, "senders",        16,  0);
pomelo_soak_watch(&soak, "receivers",      16,  0);
pomelo_soak_watch(&soak, "packets",        32,  0);
pomelo_soak_watch(&soak, "crypto_contexts", 4,  0);
pomelo_soak_watch(&soak, "buffers",        32,  0);
pomelo_soak_start(&soak);
// uv_run(...); uv_loop_close(...);
pomelo_soak_finish(&soak);
// then the cloned destroy + the two check_resource_leak calls from protocol-test.c
```

Change `main`'s signature to `int main(int argc, char ** argv)`.

- [ ] **Step 2: Add the CMake target**

Inside `if (POMELO_BUILD_SOAK)`:

```cmake
        add_executable(${POMELO_SOAK_PROTOCOL} test/soak-test/soak-protocol.c)
        target_include_directories(${POMELO_SOAK_PROTOCOL} PRIVATE ${POMELO_TEST_INCLUDE})
        target_link_libraries(${POMELO_SOAK_PROTOCOL} PRIVATE
            ${POMELO_BASE}
            ${POMELO_PROTOCOL}
            ${POMELO_UTILS}
            ${POMELO_PLATFORM_UV}
            ${POMELO_CRYPTO}
            ${POMELO_ADAPTER_DEFAULT}
            ${POMELO_TEST_STATISTIC_CHECK}
            ${LIB_SODIUM}
            ${LIB_UV}
        )
        target_compile_options(${POMELO_SOAK_PROTOCOL} PRIVATE ${POMELO_COMPILE_FLAGS})
```

- [ ] **Step 3: Build**

Run: `cmake -S . -B build -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DPOMELO_BUILD_SOAK=ON && cmake --build build --target pomelo-soak-protocol -j`
Expected: builds clean. (Resolve the exact buffer-view send idiom against `protocol-test.c:264-290` if the send call does not compile first time.)

- [ ] **Step 4: Run — expect PASS**

Run: `POMELO_SOAK_SECONDS=5 ./build/pomelo-soak-protocol`
Expected: `[soak] ==== PASS ====`, exit 0, `packets` gauge flat around a small steady value, `iterations` well above 10.

- [ ] **Step 5: Soak revert-proof**

Temporarily make the server echo handler send **twice** (duplicate `pomelo_protocol_peer_send`) without the client decrementing for the extra — or simpler: in the client-receive branch, comment out `proto_in_flight--;` so the pump stops refilling and `iterations` stalls below the floor. Rebuild + run → `FAIL progress`. Restore exactly, rebuild + run → PASS. Confirm `git diff` clean.

- [ ] **Step 6: Commit**

```bash
git add test/soak-test/soak-protocol.c CMakeLists.txt
git commit -m "test(soak): protocol packet-path steady-state soak

Co-Authored-By: claude-flow <ruv@ruv.net>"
```

---

## Task 4: `soak-api` — API-layer sustained traffic

**Files:**
- Create: `test/soak-test/soak-api.c`
- Modify: `CMakeLists.txt` (inside `if (POMELO_BUILD_SOAK)`)

Clone the setup/teardown and global callbacks from `test/api-test/api-basic-test.c` (`main` lines ~148-231 plus the `pomelo_socket_on_*`, `pomelo_session_on_cleanup`, `pomelo_channel_on_cleanup`, `pomelo_socket_on_send_result` globals). Keep one server + one client session; after connect, sustain `pomelo_session_send` on all three channel modes continuously. Build the connect token with `init_connect_token()` cloned from that file.

- [ ] **Step 1: Write the driver**

Create `test/soak-test/soak-api.c`. Soak-specific additions:

```c
#include "soak-test/soak-common.h"

#define SOAK_API_INFLIGHT 6
#define CH_RELIABLE   0
#define CH_SEQUENCED  1
#define CH_UNRELIABLE 2

static pomelo_soak_t soak;
static pomelo_session_t * client_session = NULL; // captured in client on_connected
static size_t api_in_flight = 0;
static const uint8_t api_payload[8] = {1,2,3,4,5,6,7,8};

static const pomelo_channel_mode api_modes[3] = {
    POMELO_CHANNEL_MODE_RELIABLE,
    POMELO_CHANNEL_MODE_SEQUENCED,
    POMELO_CHANNEL_MODE_UNRELIABLE
};
static size_t api_mode_index = 0;

static void api_send_one(void) {
    if (soak.stopped || !client_session) return;
    size_t ch = api_mode_index % 3;
    api_mode_index++;
    pomelo_channel_t * channel = pomelo_session_get_channel(client_session, ch);
    pomelo_check(channel != NULL);
    pomelo_check(pomelo_channel_set_mode(channel, api_modes[ch]) == 0);
    pomelo_message_t * message = pomelo_context_acquire_message(context);
    pomelo_check(message != NULL);
    pomelo_message_write(message, api_payload, sizeof(api_payload));
    pomelo_session_send(client_session, ch, message, NULL);
    api_in_flight++;
}

static void api_pump(void) {
    while (!soak.stopped && client_session && api_in_flight < SOAK_API_INFLIGHT)
        api_send_one();
}
```

Wire callbacks:
- client `on_connected(session)`: `client_session = session;` then `api_pump();`
- `pomelo_socket_on_send_result(...)`: `if (api_in_flight) api_in_flight--; pomelo_soak_tick(&soak); api_pump();`
- server `on_received`: no-op beyond what the cloned handler needs (the send-result on the client side drives the pump).
- Do not stop sockets on disconnect; the soak timer drives stop.

Sample + stop + main:

```c
static void soak_sample(void * ud, uint64_t * values, size_t n) {
    (void) ud; (void) n;
    pomelo_statistic_t s;
    pomelo_context_statistic(context, &s);
    values[0] = s.api.messages;
    values[1] = s.api.builtin_sessions;
    values[2] = s.api.builtin_channels;
    values[3] = s.delivery.parcels;
    values[4] = s.delivery.senders;
    values[5] = s.delivery.receivers;
    values[6] = s.buffer.buffers;
    values[7] = s.allocator.allocated_bytes;
}

static void soak_stop(void * ud) {
    (void) ud;
    pomelo_socket_stop(server);
    pomelo_socket_stop(client);
}

// main(), before uv_run:
memset(&soak, 0, sizeof(soak));
soak.platform = platform;
soak.sample = soak_sample;
soak.stop = soak_stop;
soak.progress_floor = 10;
pomelo_soak_config_from_args(&soak, argc, argv);
pomelo_soak_watch(&soak, "messages",  16, 0);
pomelo_soak_watch(&soak, "sessions",   4, 0);
pomelo_soak_watch(&soak, "channels",  16, 0);
pomelo_soak_watch(&soak, "parcels",   16, 0);
pomelo_soak_watch(&soak, "d_senders", 16, 0);
pomelo_soak_watch(&soak, "d_receivers", 16, 0);
pomelo_soak_watch(&soak, "buffers",   32, 0);
pomelo_soak_watch(&soak, "alloc_bytes", 0, 25);
pomelo_soak_start(&soak);
// uv_run; uv_loop_close (or the cloned run idiom)
pomelo_soak_finish(&soak);
// cloned destroy + pomelo_statistic_check_resource_leak(&statistic)
```

Note: `api-basic-test.c` creates the platform via `pomelo_test_platform_create(allocator)` and compiles `test/platform-test/platform-uv.c` — the CMake target below includes that source. Change `main` to `int main(int argc, char ** argv)`.

- [ ] **Step 2: Add the CMake target**

Inside `if (POMELO_BUILD_SOAK)`:

```cmake
        add_executable(${POMELO_SOAK_API}
            test/soak-test/soak-api.c
            test/platform-test/platform-uv.c
        )
        target_include_directories(${POMELO_SOAK_API} PRIVATE ${POMELO_TEST_INCLUDE})
        target_link_libraries(${POMELO_SOAK_API} PRIVATE
            ${POMELO_BASE}
            ${POMELO_PROTOCOL}
            ${POMELO_UTILS}
            ${POMELO_PLATFORM_UV}
            ${POMELO_CRYPTO}
            ${POMELO_DELIVERY}
            ${POMELO_API}
            ${POMELO_ADAPTER_DEFAULT}
            ${POMELO_TEST_STATISTIC_CHECK}
            ${LIB_UV}
            ${LIB_SODIUM}
        )
        target_compile_options(${POMELO_SOAK_API} PRIVATE ${POMELO_COMPILE_FLAGS})
```

- [ ] **Step 3: Build**

Run: `cmake -S . -B build -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DPOMELO_BUILD_SOAK=ON && cmake --build build --target pomelo-soak-api -j`
Expected: builds clean. (Confirm `pomelo_message_write` is the correct writer name against `api-basic-test.c`; reuse whatever that file uses to fill a message.)

- [ ] **Step 4: Run — expect PASS**

Run: `POMELO_SOAK_SECONDS=5 ./build/pomelo-soak-api`
Expected: `[soak] ==== PASS ====`, exit 0, `messages` gauge flat, `iterations` above 10.

- [ ] **Step 5: Soak revert-proof**

Temporarily comment out the `if (api_in_flight) api_in_flight--;` in `pomelo_socket_on_send_result` → the pump drains and `iterations` stalls → `FAIL progress`. Alternatively, inject a message leak by acquiring a second message per send without sending it → `messages` high-water climbs → `GROWTH`. Confirm FAIL, restore exactly, rebuild + run → PASS, `git diff` clean.

- [ ] **Step 6: Commit**

```bash
git add test/soak-test/soak-api.c CMakeLists.txt
git commit -m "test(soak): API-layer sustained-traffic soak

Co-Authored-By: claude-flow <ruv@ruv.net>"
```

---

## Task 5: `soak-engine` — whole-engine combo (traffic + churn)

**Files:**
- Create: `test/soak-test/soak-engine.c`
- Modify: `CMakeLists.txt` (inside `if (POMELO_BUILD_SOAK)`)

Clone setup from `test/api-test/api-basic-test.c`. One server. A **fixed pool** of client sockets: `POOL = 4`. Two of them sustain mixed-mode traffic (like soak-api) for the whole run; the other two **churn**: connect → send a few → stop → recreate with a fresh connect token → reconnect, repeatedly. This drives every layer at once and watches the full `pomelo_statistic_t`.

Each client is created with its own connect token; refresh `token.create_timestamp = pomelo_platform_now(platform)` before each encode so a recreated client is accepted. Track per-client role (persistent vs churning) in a small array so the global `pomelo_socket_on_*` callbacks can branch by looking up which socket fired.

- [ ] **Step 1: Write the driver**

Create `test/soak-test/soak-engine.c`. Structure:

```c
#include "soak-test/soak-common.h"

#define SOAK_ENGINE_POOL      4
#define SOAK_ENGINE_PERSIST   2   // clients[0..1] persist; clients[2..3] churn
#define SOAK_ENGINE_INFLIGHT  4
#define SOAK_ENGINE_CHURN_SENDS 5 // sends before a churning client disconnects

typedef struct {
    pomelo_socket_t * socket;
    pomelo_session_t * session;
    int churning;        // 1 for churn clients
    size_t in_flight;
    size_t sends_done;   // for churn cycle
} soak_client_t;

static soak_client_t clients[SOAK_ENGINE_POOL];
static pomelo_socket_t * server = NULL;
static pomelo_soak_t soak;
static const uint8_t payload[8] = {9,8,7,6,5,4,3,2};
```

Helpers (concrete bodies written against the api-basic idiom):
- `make_token_and_connect(soak_client_t * c)` — create a socket (`pomelo_socket_create`), refresh+encode a connect token, `pomelo_socket_connect(c->socket, connect_token)`.
- `engine_send(soak_client_t * c)` — acquire message, write `payload`, `pomelo_session_send` cycling the 3 modes (as in soak-api), `c->in_flight++`.
- `engine_pump(soak_client_t * c)` — while `!soak.stopped && c->session && c->in_flight < SOAK_ENGINE_INFLIGHT` → `engine_send(c)`.

Callback wiring (branch by matching the fired socket against `clients[i].socket`):
- client `on_connected`: set `c->session`; `c->sends_done = 0`; `engine_pump(c)`.
- `on_send_result`: `c->in_flight--`; `pomelo_soak_tick(&soak)`; if churn and `++c->sends_done >= SOAK_ENGINE_CHURN_SENDS` → `pomelo_socket_stop(c->socket)` (begin disconnect); else `engine_pump(c)`.
- client `on_stopped` / `on_disconnected`: if this is a churn client and `!soak.stopped` → destroy + recreate via `make_token_and_connect(c)` (reconnect); if `soak.stopped` do nothing (drain).
- server `on_received`: no-op beyond cloned needs.

Sample watches the entire struct:

```c
static void soak_sample(void * ud, uint64_t * values, size_t n) {
    (void) ud; (void) n;
    pomelo_statistic_t s;
    pomelo_context_statistic(context, &s);
    values[0] = s.protocol.peers;
    values[1] = s.protocol.packets;
    values[2] = s.delivery.parcels;
    values[3] = s.delivery.senders;
    values[4] = s.delivery.receivers;
    values[5] = s.api.messages;
    values[6] = s.api.builtin_sessions;
    values[7] = s.api.builtin_channels;
    values[8] = s.buffer.buffers;
    values[9] = s.allocator.allocated_bytes;
}

static void soak_stop(void * ud) {
    (void) ud;
    for (size_t i = 0; i < SOAK_ENGINE_POOL; i++)
        if (clients[i].socket) pomelo_socket_stop(clients[i].socket);
    pomelo_socket_stop(server);
}
```

Main: create server (`pomelo_socket_listen`), create the 4 clients via `make_token_and_connect`, set slack generously (peers/sessions up to `POOL`, so slack 8; packets/messages 32; `alloc_bytes` pct 25), `progress_floor = 10`, `pomelo_soak_start`, run, `pomelo_soak_finish`, then cloned destroy of server + all clients + `pomelo_statistic_check_resource_leak(&statistic)`.

Watch registration order must match `soak_sample` above:

```c
pomelo_soak_watch(&soak, "peers",       8,  0);
pomelo_soak_watch(&soak, "packets",     48, 0);
pomelo_soak_watch(&soak, "parcels",     32, 0);
pomelo_soak_watch(&soak, "d_senders",   32, 0);
pomelo_soak_watch(&soak, "d_receivers", 32, 0);
pomelo_soak_watch(&soak, "messages",    32, 0);
pomelo_soak_watch(&soak, "sessions",    8,  0);
pomelo_soak_watch(&soak, "channels",    32, 0);
pomelo_soak_watch(&soak, "buffers",     64, 0);
pomelo_soak_watch(&soak, "alloc_bytes", 0,  25);
```

- [ ] **Step 2: Add the CMake target**

Inside `if (POMELO_BUILD_SOAK)` (same link set as `soak-api`):

```cmake
        add_executable(${POMELO_SOAK_ENGINE}
            test/soak-test/soak-engine.c
            test/platform-test/platform-uv.c
        )
        target_include_directories(${POMELO_SOAK_ENGINE} PRIVATE ${POMELO_TEST_INCLUDE})
        target_link_libraries(${POMELO_SOAK_ENGINE} PRIVATE
            ${POMELO_BASE}
            ${POMELO_PROTOCOL}
            ${POMELO_UTILS}
            ${POMELO_PLATFORM_UV}
            ${POMELO_CRYPTO}
            ${POMELO_DELIVERY}
            ${POMELO_API}
            ${POMELO_ADAPTER_DEFAULT}
            ${POMELO_TEST_STATISTIC_CHECK}
            ${LIB_UV}
            ${LIB_SODIUM}
        )
        target_compile_options(${POMELO_SOAK_ENGINE} PRIVATE ${POMELO_COMPILE_FLAGS})
```

- [ ] **Step 3: Build**

Run: `cmake -S . -B build -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DPOMELO_BUILD_SOAK=ON && cmake --build build --target pomelo-soak-engine -j`
Expected: builds clean.

- [ ] **Step 4: Run — expect PASS**

Run: `POMELO_SOAK_SECONDS=8 ./build/pomelo-soak-engine`
Expected: `[soak] ==== PASS ====`, exit 0. The churn clients cause `peers`/`sessions` to oscillate but stay ≤ baseline+slack; `alloc_bytes` flat within 25%.

- [ ] **Step 5: Soak revert-proof**

Temporarily skip destroying a churn client before recreating it (leak a socket per cycle) → `sessions`/`peers`/`alloc_bytes` climb → `GROWTH` FAIL. Restore exactly → PASS. Confirm `git diff` clean.

- [ ] **Step 6: Commit**

```bash
git add test/soak-test/soak-engine.c CMakeLists.txt
git commit -m "test(soak): whole-engine combo soak (sustained traffic + connection churn)

Co-Authored-By: claude-flow <ruv@ruv.net>"
```

---

## Task 6: Document the soak suite

**Files:**
- Modify: `docs/TEST_REVIEW.md` (add a "Soak / endurance tests" subsection)
- Create: `test/soak-test/README.md`

- [ ] **Step 1: Write `test/soak-test/README.md`**

```markdown
# Soak / endurance tests

Standalone, long-running endurance binaries. **Not** registered with CTest —
build with `-DPOMELO_BUILD_SOAK=ON` and run manually.

| Binary | Scope |
|--------|-------|
| `pomelo-soak-delivery` | Delivery endpoints under continuous mixed-mode parcel traffic. |
| `pomelo-soak-protocol` | Protocol client↔server, sustained packet path. |
| `pomelo-soak-api`      | API socket/session, sustained mixed-mode sends. |
| `pomelo-soak-engine`   | Whole engine: persistent traffic + connection churn. |

## Build & run

```bash
cmake -S . -B build -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DPOMELO_BUILD_SOAK=ON
cmake --build build -j
POMELO_SOAK_SECONDS=300 ./build/pomelo-soak-engine      # 5-minute run
./build/pomelo-soak-delivery 600                        # argv overrides seconds
```

Env: `POMELO_SOAK_SECONDS` (default 30), `POMELO_SOAK_WARMUP_SECONDS` (default 3).

## What they assert

Each samples the occupancy gauges in `pomelo_statistic_t` every 250 ms and, after
warmup, asserts no gauge climbs past `baseline + slack` (growth guard), that work
kept flowing (progress guard), and zero resource leak at teardown. A PASS prints
`[soak] ==== PASS ====` and exits 0; any failure aborts non-zero.
```

- [ ] **Step 2: Add a subsection to `docs/TEST_REVIEW.md`**

Append under the existing test-inventory section:

```markdown
### Soak / endurance tests (manual, not CTest)

Four standalone binaries behind `-DPOMELO_BUILD_SOAK=ON` (`test/soak-test/`):
`pomelo-soak-delivery`, `-protocol`, `-api`, `-engine`. Shared harness
`soak-common.h` samples `pomelo_statistic_t` occupancy gauges and asserts a
steady-state invariant (no gauge grows past baseline+slack after warmup) plus
forward-progress and zero-leak. Run manually (nightly / on-demand); duration via
`POMELO_SOAK_SECONDS`. See `test/soak-test/README.md`.
```

- [ ] **Step 3: Commit**

```bash
git add docs/TEST_REVIEW.md test/soak-test/README.md
git commit -m "docs(soak): document the standalone soak-test suite

Co-Authored-By: claude-flow <ruv@ruv.net>"
```

---

## Self-review notes (for the implementer)

- **Build flag:** always pass `-DCMAKE_POLICY_VERSION_MINIMUM=3.5` (old nested deps need it) and `-DPOMELO_BUILD_SOAK=ON`.
- **Global callbacks:** each soak `.c` is its own executable, so it must define the layer's global `pomelo_*_on_*` callbacks exactly once — clone the full set from the named source file; an undefined one is a link error, a duplicate is also a link error.
- **Sample order:** the order of `pomelo_soak_watch()` calls MUST match the `values[i]` assignments in that driver's `soak_sample`. A mismatch mislabels counters (not a crash, but a misleading report).
- **Quiet runs:** strip per-packet `printf` from cloned callbacks; only the harness `[soak]` lines should print, or a long run floods the terminal.
- **Stop discipline:** the sample timer is stopped *inside* the loop (`pomelo_soak_on_sample`), and `soak_stop` stops sockets/endpoints inside the loop — same handle-close discipline as the Task 22 watchdog, so no handle leaks.
- **If a driver's exact send idiom differs** (buffer-view construction for `pomelo_protocol_peer_send`, or the message writer name), resolve it against the named clone-source file during Step 3 build; do not invent symbols.
```
