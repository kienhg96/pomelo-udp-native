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
    /* Keep a real warmup even for short runs so the baseline is taken after the
     * pools reach steady state (a too-early baseline causes false growth). */
    if (warmup >= seconds) warmup = seconds / 2;

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
