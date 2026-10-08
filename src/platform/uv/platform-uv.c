#include <assert.h>
#include <string.h>
#include "uv.h"
#include "utils/list.h"
#include "platform-uv.h"
#include "executor.h"
#include "worker.h"
#include "timer.h"
#include "udp.h"


void pomelo_platform_uv_set_extra(
    pomelo_platform_uv_t * platform,
    void * data
) {
    assert(platform != NULL);
    pomelo_extra_set(platform->extra, data);
}


void * pomelo_platform_uv_get_extra(pomelo_platform_uv_t * platform) {
    assert(platform != NULL);
    return pomelo_extra_get(platform->extra);
}


pomelo_platform_t * pomelo_platform_uv_create(
    pomelo_platform_uv_options_t * options
) {
    assert(options != NULL);
    if (!options->uv_loop) {
        return NULL;
    }

    pomelo_allocator_t * allocator = options->allocator;
    if (!allocator) {
        allocator = pomelo_allocator_default();
    }

    pomelo_platform_uv_t * platform =
        pomelo_allocator_malloc_t(allocator, pomelo_platform_uv_t);
    if (!platform) return NULL; // Failed to allocate new platform

    memset(platform, 0, sizeof(pomelo_platform_uv_t));
    pomelo_extra_set(platform->extra, NULL);

    uv_loop_t * uv_loop = options->uv_loop;
    platform->allocator = allocator;
    platform->uv_loop = uv_loop;

    // Initialize idle handle for close
    uv_idle_init(uv_loop, &platform->idle_close);
    platform->idle_close.data = platform;

    platform->worker_mgr =
        pomelo_platform_uv_worker_mgr_create(platform, allocator, uv_loop);
    if (!platform->worker_mgr) {
        pomelo_platform_uv_destroy((pomelo_platform_t *) platform);
        return NULL;
    }

    platform->executor_mgr =
        pomelo_platform_uv_executor_mgr_create(
            platform, allocator, uv_loop
        );
    if (!platform->executor_mgr) {
        pomelo_platform_uv_destroy((pomelo_platform_t *) platform);
        return NULL;
    }

    platform->udp_mgr = pomelo_platform_uv_udp_mgr_create(
        platform, allocator, uv_loop
    );
    if (!platform->udp_mgr) {
        pomelo_platform_uv_destroy((pomelo_platform_t *) platform);
        return NULL;
    }

    // Create timer manager
    platform->timer_mgr = pomelo_platform_uv_timer_mgr_create(
        platform, allocator, uv_loop
    );
    if (!platform->timer_mgr) {
        pomelo_platform_uv_destroy((pomelo_platform_t *) platform);
        return NULL;
    }

    return (pomelo_platform_t *) platform;
}


void pomelo_platform_uv_destroy(pomelo_platform_t * platform) {
    assert(platform != NULL);
    pomelo_platform_uv_t * uv_platform = (pomelo_platform_uv_t *) platform;

    if (uv_platform->timer_mgr) {
        pomelo_platform_uv_timer_mgr_destroy(uv_platform->timer_mgr);
        uv_platform->timer_mgr = NULL;
    }

    if (uv_platform->executor_mgr) {
        pomelo_platform_uv_executor_mgr_destroy(uv_platform->executor_mgr);
        uv_platform->executor_mgr = NULL;
    }

    if (uv_platform->worker_mgr) {
        pomelo_platform_uv_worker_mgr_destroy(uv_platform->worker_mgr);
        uv_platform->worker_mgr = NULL;
    }

    if (uv_platform->udp_mgr) {
        pomelo_platform_uv_udp_mgr_destroy(uv_platform->udp_mgr);
        uv_platform->udp_mgr = NULL;
    }

    pomelo_allocator_free(uv_platform->allocator, uv_platform);
}


void pomelo_platform_uv_statistic(
    pomelo_platform_t * platform,
    pomelo_statistic_platform_uv_t * statistic
) {
    assert(platform != NULL);
    assert(statistic != NULL);

    pomelo_platform_uv_t * uv_platform = (pomelo_platform_uv_t *) platform;

    pomelo_platform_uv_timer_mgr_statistic(uv_platform->timer_mgr, statistic);
    pomelo_platform_uv_udp_mgr_statistic(uv_platform->udp_mgr, statistic);
    pomelo_platform_uv_executor_mgr_statistic(
        uv_platform->executor_mgr, statistic
    );
    pomelo_platform_uv_worker_mgr_statistic(uv_platform->worker_mgr, statistic);
}


uv_loop_t * pomelo_platform_uv_get_uv_loop(pomelo_platform_t * platform) {
    assert(platform != NULL);
    return ((pomelo_platform_uv_t *) platform)->uv_loop;
}


static void idle_close_complete(uv_handle_t * handle) {
    pomelo_platform_uv_t * platform = handle->data;
    assert(platform != NULL);

    platform->closed_components |= POMELO_PLATFORM_UV_COMPONENT_IDLE;
    pomelo_platform_uv_check_closed(platform);
}


static void idle_close_cb(uv_idle_t * handle) {
    pomelo_platform_uv_t * platform = handle->data;
    assert(platform != NULL);

    // Stop and close the idle handle itself so the loop can be torn down.
    uv_idle_stop(handle);
    uv_close((uv_handle_t *) handle, idle_close_complete);

    pomelo_platform_uv_udp_mgr_close(platform->udp_mgr);
    pomelo_platform_uv_timer_mgr_close(platform->timer_mgr);
    pomelo_platform_uv_executor_mgr_close(platform->executor_mgr);
    pomelo_platform_uv_worker_mgr_close(platform->worker_mgr);
}


int pomelo_platform_uv_close(
    pomelo_platform_uv_t * platform,
    pomelo_platform_callback callback
) {
    assert(platform != NULL);
    if (platform->flags & POMELO_PLATFORM_UV_FLAG_CLOSING) {
        return -1; // The platform is closing
    }

    platform->flags |= POMELO_PLATFORM_UV_FLAG_CLOSING;
    platform->closed_callback = callback;
    platform->closed_components = 0;

    uv_idle_start(&platform->idle_close, idle_close_cb);
    return 0;
}


void pomelo_platform_uv_executor_mgr_on_closed(
    pomelo_platform_uv_executor_mgr_t * manager
) {
    assert(manager != NULL);
    pomelo_platform_uv_t * platform = manager->platform;
    assert(platform != NULL);

    platform->closed_components |= POMELO_PLATFORM_UV_COMPONENT_THREADSAFE;
    pomelo_platform_uv_check_closed(platform);
}


void pomelo_platform_uv_worker_mgr_on_closed(
    pomelo_platform_uv_worker_mgr_t * manager
) {
    assert(manager != NULL);
    pomelo_platform_uv_t * platform = manager->platform;
    assert(platform != NULL);

    platform->closed_components |= POMELO_PLATFORM_UV_COMPONENT_WORKER;
    pomelo_platform_uv_check_closed(platform);
}


void pomelo_platform_uv_udp_mgr_on_closed(
    pomelo_platform_uv_udp_mgr_t * manager
) {
    assert(manager != NULL);
    pomelo_platform_uv_t * platform = manager->platform;
    assert(platform != NULL);

    platform->closed_components |= POMELO_PLATFORM_UV_COMPONENT_UDP;
    pomelo_platform_uv_check_closed(platform);
}


void pomelo_platform_uv_timer_mgr_on_closed(
    pomelo_platform_uv_timer_mgr_t * manager
) {
    assert(manager != NULL);
    pomelo_platform_uv_t * platform = manager->platform;
    assert(platform != NULL);

    platform->closed_components |= POMELO_PLATFORM_UV_COMPONENT_TIMER;
    pomelo_platform_uv_check_closed(platform);
}


void pomelo_platform_uv_check_closed(pomelo_platform_uv_t * platform) {
    assert(platform != NULL);

    if (platform->closed_components == POMELO_PLATFORM_UV_COMPONENT_ALL) {
        platform->flags &= ~POMELO_PLATFORM_UV_FLAG_CLOSING;
        if (platform->closed_callback) {
            platform->closed_callback((pomelo_platform_t *) platform);
        }
    }
}
