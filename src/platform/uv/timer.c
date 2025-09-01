#include <assert.h>
#include <stdbool.h>
#include "timer.h"


pomelo_platform_uv_timer_mgr_t * pomelo_platform_uv_timer_mgr_create(
    pomelo_platform_uv_t * platform,
    pomelo_allocator_t * allocator,
    uv_loop_t * uv_loop
) {
    assert(allocator != NULL);
    assert(uv_loop != NULL);

    pomelo_platform_uv_timer_mgr_t * manager = pomelo_allocator_malloc(
        allocator,
        sizeof(pomelo_platform_uv_timer_mgr_t)
    );

    if (!manager) {
        return NULL;
    }

    memset(manager, 0, sizeof(pomelo_platform_uv_timer_mgr_t));
    manager->platform = platform;
    manager->allocator = allocator;
    manager->uv_loop = uv_loop;

    // Create active timers list
    pomelo_list_options_t list_options = {
        .allocator = allocator,
        .element_size = sizeof(pomelo_platform_uv_timer_t *)
    };
    manager->timers = pomelo_list_create(&list_options);
    if (!manager->timers) {
        pomelo_platform_uv_timer_mgr_destroy(manager);
        return NULL;
    }

    // Create timer pool
    pomelo_pool_root_options_t pool_options = {
        .allocator = allocator,
        .element_size = sizeof(pomelo_platform_uv_timer_t),
        .zero_init = true
    };
    manager->timer_pool = pomelo_pool_root_create(&pool_options);

    if (!manager->timer_pool) {
        pomelo_platform_uv_timer_mgr_destroy(manager);
        return NULL;
    }

    return manager;
}



void pomelo_platform_uv_timer_mgr_destroy(
    pomelo_platform_uv_timer_mgr_t * manager
) {
    assert(manager != NULL);
    pomelo_allocator_t * allocator = manager->allocator;

    if (manager->timers) {
        pomelo_list_destroy(manager->timers);
        manager->timers = NULL;
    }

    if (manager->timer_pool) {
        pomelo_pool_destroy(manager->timer_pool);
        manager->timer_pool = NULL;
    }

    pomelo_allocator_free(allocator, manager);
}


void pomelo_platform_uv_timer_mgr_statistic(
    pomelo_platform_uv_timer_mgr_t * manager,
    pomelo_statistic_platform_uv_t * statistic
) {
    assert(manager != NULL);
    assert(statistic != NULL);

    statistic->timers = manager->timers->size;
}


void pomelo_platform_uv_timer_mgr_close(
    pomelo_platform_uv_timer_mgr_t * manager
) {
    assert(manager != NULL);

    pomelo_platform_uv_timer_t * timer = NULL;
    pomelo_list_iterator_t it;
    pomelo_list_iterator_init(&it, manager->timers);
    while (pomelo_list_iterator_next(&it, &timer) == 0) {
        pomelo_platform_uv_timer_stop_ex(timer);
    }

    pomelo_platform_uv_timer_mgr_check_closed(manager);
}


int pomelo_platform_uv_timer_start(
    pomelo_platform_uv_t * platform,
    pomelo_platform_timer_entry entry,
    uint64_t timeout_ms,
    uint64_t repeat_ms,
    void * data,
    pomelo_platform_handle_t * timer_handle
) {
    assert(platform != NULL);
    assert(entry != NULL);

    pomelo_platform_uv_timer_mgr_t * manager = platform->timer_mgr;
    pomelo_platform_uv_timer_t * timer =
        pomelo_pool_acquire(manager->timer_pool, NULL);
    if (!timer) return -1; // Cannot allocate new timer

    timer->manager = manager;
    timer->data = data;
    timer->entry = entry;
    timer->is_repeat = (repeat_ms != 0);
    timer->is_running = true;

    timer->list_entry = pomelo_list_push_back(manager->timers, timer);
    if (!timer->list_entry) {
        // Cannot add new timer to active list
        pomelo_pool_release(manager->timer_pool, timer);
        return -1;
    }

    uv_timer_init(manager->uv_loop, &timer->uv_timer);
    timer->uv_timer.data = timer;

    int ret = uv_timer_start(
        &timer->uv_timer,
        pomelo_platform_uv_timer_callback,
        timeout_ms,
        repeat_ms
    );

    if (ret < 0) {
        // Cannot start the timer
        pomelo_list_remove(manager->timers, timer->list_entry);
        pomelo_pool_release(manager->timer_pool, timer);
        return -1;
    }

    // Attach the timer to the handle
    timer->handle = timer_handle;
    if (timer_handle) {
        timer_handle->type = POMELO_PLATFORM_HANDLE_TIMER;
        timer_handle->data = timer;
    }

    return 0;
}


void pomelo_platform_uv_timer_stop(
    pomelo_platform_uv_t * platform,
    pomelo_platform_handle_t * timer_handle
) {
    (void) platform;
    assert(timer_handle != NULL);

    if (!timer_handle->data ||
        timer_handle->type != POMELO_PLATFORM_HANDLE_TIMER
    ) return; // Invalid

    pomelo_platform_uv_timer_stop_ex(timer_handle->data);
}


/* -------------------------------------------------------------------------- */
/*                               Private APIs                                 */
/* -------------------------------------------------------------------------- */


void pomelo_platform_uv_timer_mgr_check_closed(
    pomelo_platform_uv_timer_mgr_t * manager
) {
    assert(manager != NULL);
    if (manager->timers->size == 0) {
        pomelo_platform_uv_timer_mgr_on_closed(manager);
    }
}


/// @brief Callback for when the timer handle is closed
static void uv_timer_stop_complete(uv_handle_t * handle) {
    pomelo_platform_uv_timer_t * timer = handle->data;

    // Remove timer from active list and release it
    pomelo_platform_uv_timer_mgr_t * manager = timer->manager;
    pomelo_list_remove(manager->timers, timer->list_entry);
    pomelo_pool_release(manager->timer_pool, timer);

    pomelo_platform_uv_timer_mgr_check_closed(manager);
}


int pomelo_platform_uv_timer_stop_ex(pomelo_platform_uv_timer_t * timer) {
    assert(timer != NULL);
    if (!timer->is_running) {
        return 0; // The timer is not running. Nothing to do
    }

    // Stop the UV timer
    timer->is_running = false;
    uv_close((uv_handle_t *) &timer->uv_timer, uv_timer_stop_complete);

    // Detach the handle
    pomelo_platform_handle_t * handle = timer->handle;
    if (handle && handle->data == timer) {
        // Only detach the handle if the handle is holding this timer
        handle->type = POMELO_PLATFORM_HANDLE_NONE;
        handle->data = NULL;
        timer->handle = NULL;
    }

    return 0;
}


void pomelo_platform_uv_timer_callback(uv_timer_t * uv_timer) {
    assert(uv_timer->data != NULL);

    pomelo_platform_uv_timer_t * timer = uv_timer->data;
    pomelo_platform_timer_entry entry = timer->entry;
    void * data = timer->data;

    if (!timer->is_repeat) {
        // Timer is not repeating, stop running
        pomelo_platform_uv_timer_stop_ex(timer);
    }

    entry(data);
}
