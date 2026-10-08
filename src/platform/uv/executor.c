#include <assert.h>
#include <string.h>
#include "executor.h"


/* -------------------------------------------------------------------------- */
/*                                Public APIs                                 */
/* -------------------------------------------------------------------------- */


pomelo_platform_uv_executor_mgr_t * pomelo_platform_uv_executor_mgr_create(
    pomelo_platform_uv_t * platform,
    pomelo_allocator_t * allocator,
    uv_loop_t * uv_loop
) {
    assert(platform != NULL);
    assert(allocator != NULL);
    assert(uv_loop != NULL);

    pomelo_platform_uv_executor_mgr_t * manager = pomelo_allocator_malloc_t(
        allocator,
        pomelo_platform_uv_executor_mgr_t
    );
    if (!manager) {
        return NULL;
    }
    memset(manager, 0, sizeof(pomelo_platform_uv_executor_mgr_t));
    manager->platform = platform;
    manager->allocator = allocator;
    manager->uv_loop = uv_loop;

    pomelo_atomic_uint64_store(&manager->task_counter, 0);

    // Create tasks pool
    pomelo_pool_root_options_t pool_options;
    memset(&pool_options, 0, sizeof(pomelo_pool_root_options_t));
    pool_options.allocator = allocator;
    pool_options.element_size = sizeof(pomelo_platform_uv_task_executor_t);
    pool_options.zero_init = true;
    pool_options.synchronized = true;
    manager->task_pool = pomelo_pool_root_create(&pool_options);
    if (!manager->task_pool) {
        pomelo_platform_uv_executor_mgr_destroy(manager);
        return NULL;
    }

    // Create executor pool
    memset(&pool_options, 0, sizeof(pomelo_pool_root_options_t));
    pool_options.allocator = allocator;
    pool_options.element_size = sizeof(pomelo_platform_executor_t);
    pool_options.synchronized = true;
    pool_options.alloc_data = manager;
    pool_options.on_alloc = (pomelo_pool_alloc_cb)
        pomelo_platform_executor_on_alloc;
    pool_options.on_free = (pomelo_pool_free_cb)
        pomelo_platform_executor_on_free;
    manager->executor_pool = pomelo_pool_root_create(&pool_options);
    if (!manager->executor_pool) {
        pomelo_platform_uv_executor_mgr_destroy(manager);
        return NULL;
    }

    // Create executors list
    pomelo_list_options_t list_options = {
        .allocator = allocator,
        .element_size = sizeof(pomelo_platform_executor_t *),
        .synchronized = true
    };
    manager->executors = pomelo_list_create(&list_options);
    if (!manager->executors) {
        pomelo_platform_uv_executor_mgr_destroy(manager);
        return NULL;
    }
    return manager;
}


void pomelo_platform_uv_executor_mgr_destroy(
    pomelo_platform_uv_executor_mgr_t * manager
) {
    assert(manager != NULL);
    pomelo_allocator_t * allocator = manager->allocator;

    if (manager->task_pool) {
        pomelo_pool_destroy(manager->task_pool);
        manager->task_pool = NULL;
    }

    if (manager->executor_pool) {
        pomelo_pool_destroy(manager->executor_pool);
        manager->executor_pool = NULL;
    }

    if (manager->executors) {
        pomelo_list_destroy(manager->executors);
        manager->executors = NULL;
    }

    pomelo_allocator_free(allocator, manager);
}


void pomelo_platform_uv_executor_mgr_close(
    pomelo_platform_uv_executor_mgr_t * manager
) {
    assert(manager != NULL);
    pomelo_list_t * executors = manager->executors;

    // Shutdown all executors
    pomelo_platform_executor_t * executor = NULL;
    pomelo_list_iterator_t it;
    pomelo_list_iterator_init(&it, executors);
    while (pomelo_list_iterator_next(&it, &executor) == 0) {
        pomelo_platform_executor_shutdown(executor, manager);
    }

    // Check close
    pomelo_platform_uv_executor_mgr_check_closed(manager);
}


void pomelo_platform_uv_executor_mgr_statistic(
    pomelo_platform_uv_executor_mgr_t * manager,
    pomelo_statistic_platform_uv_t * statistic
) {
    assert(manager != NULL);
    assert(statistic != NULL);
    statistic->threadsafe_tasks =
        pomelo_atomic_uint64_load(&manager->task_counter);
}


int pomelo_platform_uv_acquire_executor(
    pomelo_platform_uv_t * platform,
    pomelo_platform_handle_t * executor_handle
) {
    assert(platform != NULL);
    assert(executor_handle != NULL);

    pomelo_platform_uv_executor_mgr_t * manager = platform->executor_mgr;
    pomelo_platform_executor_t * executor =
        pomelo_pool_acquire(manager->executor_pool, NULL);
    if (!executor) return -1; // Failed to acquire executor

    executor->entry = pomelo_list_push_back(manager->executors, executor);
    if (!executor->entry) {
        pomelo_pool_release(manager->executor_pool, executor);
        return -1; // Failed to add executor to list
    }

    // Startup the executor
    int ret = pomelo_platform_executor_startup(executor, manager);
    if (ret < 0) {
        pomelo_list_remove(manager->executors, executor->entry);
        pomelo_pool_release(manager->executor_pool, executor);
        return -1; // Failed to startup executor
    }

    // Attach the handle
    executor_handle->type = POMELO_PLATFORM_HANDLE_EXECUTOR;
    executor_handle->data = executor;
    executor->handle = executor_handle;

    return 0;
}


/// @brief Release the threadsafe executor
static void release_threadsafe_executor(pomelo_platform_executor_t * executor) {
    assert(executor != NULL);
    pomelo_platform_executor_shutdown(executor, executor->manager);
}


void pomelo_platform_uv_release_executor(
    pomelo_platform_uv_t * platform,
    pomelo_platform_handle_t * executor_handle
) {
    assert(platform != NULL);
    assert(executor_handle != NULL);

    if (executor_handle->type != POMELO_PLATFORM_HANDLE_EXECUTOR) {
        return; // Invalid handle type
    }

    pomelo_platform_executor_t * executor = executor_handle->data;
    assert(executor != NULL);

    // Detach the handle from executor
    executor->handle = NULL;

    // Use the executor to submit the its release task
    pomelo_platform_uv_executor_submit(
        platform,
        executor_handle,
        (pomelo_platform_task_entry) release_threadsafe_executor,
        executor
    );

    // Detach the handle.
    executor_handle->type = POMELO_PLATFORM_HANDLE_NONE;
    executor_handle->data = NULL;
}


int pomelo_platform_uv_executor_submit(
    pomelo_platform_uv_t * platform,
    pomelo_platform_handle_t * executor_handle,
    pomelo_platform_task_entry entry,
    void * data
) {
    assert(platform != NULL);
    assert(executor_handle != NULL);
    assert(entry != NULL);

    if (executor_handle->type != POMELO_PLATFORM_HANDLE_EXECUTOR) {
        return -1; // Invalid handle type
    }

    pomelo_platform_executor_t * executor = executor_handle->data;
    pomelo_platform_uv_executor_mgr_t * manager = executor->manager;

    // Acquire new task
    pomelo_platform_uv_task_executor_t * task =
        pomelo_pool_acquire(manager->task_pool, NULL);
    if (!task) {
        return -1; // Failed to acquire new task
    }
    task->manager = manager;
    task->entry = entry;
    task->data = data;

    uv_mutex_t * mutex = &executor->mutex;
    int result = 0;

    /* -------------------------  Critical Begin  ----------------------- */
    uv_mutex_lock(mutex);

    // Check running state and signal the loop under the same lock that
    // shutdown uses, so submit never races a concurrent shutdown.
    if (!pomelo_atomic_int64_load(&executor->running)) {
        result = -1; // Executor stopped; must not touch the async handle
    } else {
        pomelo_list_entry_t * list_entry =
            pomelo_list_push_back(executor->tasks_front, task);
        if (!list_entry) {
            result = -1; // Failed to append to list
        } else if (uv_async_send(&executor->uv_async) < 0) {
            pomelo_list_remove(executor->tasks_front, list_entry);
            result = -1; // Failed to send signal
        }
    }

    uv_mutex_unlock(mutex);
    /* --------------------------  Critical End  ------------------------ */

    if (result != 0) {
        pomelo_platform_uv_task_executor_release(task);
        return -1;
    }

    pomelo_atomic_uint64_fetch_add(&manager->task_counter, 1);
    return 0;
}


/* -------------------------------------------------------------------------- */
/*                               Private APIs                                 */
/* -------------------------------------------------------------------------- */


void pomelo_platform_uv_executor_mgr_check_closed(
    pomelo_platform_uv_executor_mgr_t * manager
) {
    assert(manager != NULL);
    if (manager->executors->size == 0) {
        pomelo_platform_uv_executor_mgr_on_closed(manager);
        return;
    }
}


void pomelo_platform_uv_task_executor_release(
    pomelo_platform_uv_task_executor_t * task
) {
    assert(task != NULL);
    pomelo_pool_release(task->manager->task_pool, task);
}


void pomelo_platform_task_executor_async_callback(uv_async_t * async) {
    assert(async != NULL);
    pomelo_platform_executor_t * executor = async->data;

    // Set the busy flag
    executor->flags |= POMELO_UV_EXECUTOR_FLAG_BUSY;

    uv_mutex_t * mutex = &executor->mutex;
    pomelo_list_t * tasks = executor->tasks_front;

    /* ----------- Begin mutex scope ----------- */
    uv_mutex_lock(mutex);

    // Swap the tasks lists
    executor->tasks_front = executor->tasks_back;
    executor->tasks_back = tasks;

    uv_mutex_unlock(mutex);
    /* ------------ End mutex scope ------------ */

    // Update task counter
    pomelo_platform_uv_executor_mgr_t * manager = executor->manager;
    pomelo_atomic_uint64_fetch_sub(&manager->task_counter, tasks->size);

    // Execute tasks
    pomelo_platform_uv_task_executor_t * task = NULL;
    pomelo_platform_task_entry entry = NULL;
    void * data = NULL;
    while (
        !(executor->flags & POMELO_UV_EXECUTOR_FLAG_SHUTDOWN) &&
        pomelo_list_pop_front(tasks, &task) == 0
    ) {
        entry = task->entry;
        data = task->data;
        pomelo_platform_uv_task_executor_release(task);

        entry(data);
    }

    // Clear the busy flag
    executor->flags &= ~POMELO_UV_EXECUTOR_FLAG_BUSY;
    if (executor->flags & POMELO_UV_EXECUTOR_FLAG_SHUTDOWN) {
        pomelo_platform_executor_shutdown(executor, executor->manager);
    }
}


int pomelo_platform_executor_on_alloc(
    pomelo_platform_executor_t * executor,
    pomelo_platform_uv_executor_mgr_t * manager
) {
    assert(executor != NULL);
    assert(manager != NULL);

    executor->manager = manager;
    pomelo_atomic_int64_store(&executor->running, false);

    // Create tasks lists
    pomelo_list_options_t list_options = {
        .allocator = manager->allocator,
        .element_size = sizeof(pomelo_platform_uv_task_executor_t *)
    };
    executor->tasks_front = pomelo_list_create(&list_options);
    if (!executor->tasks_front) {
        return -1;
    }

    executor->tasks_back = pomelo_list_create(&list_options);
    if (!executor->tasks_back) {
        return -1;
    }

    // Initialize mutex
    uv_mutex_init_recursive(&executor->mutex);
    return 0;
}


void pomelo_platform_executor_on_free(pomelo_platform_executor_t * executor) {
    assert(executor != NULL);

    if (executor->tasks_front) {
        pomelo_list_destroy(executor->tasks_front);
        executor->tasks_front = NULL;
    }

    if (executor->tasks_back) {
        pomelo_list_destroy(executor->tasks_back);
        executor->tasks_back = NULL;
    }

    uv_mutex_destroy(&executor->mutex);
}


int pomelo_platform_executor_startup(
    pomelo_platform_executor_t * executor,
    pomelo_platform_uv_executor_mgr_t * manager
) {
    assert(executor != NULL);
    assert(manager != NULL);

    // Initialize async
    uv_async_t * async = &executor->uv_async;
    int ret = uv_async_init(
        manager->uv_loop,
        async,
        pomelo_platform_task_executor_async_callback
    );
    if (ret < 0) {
        return -1;
    }
    async->data = executor;

    pomelo_atomic_int64_store(&executor->running, true);
    return 0;
}


/// @brief Callback for when the async handle is closed
static void executor_shutdown_complete(uv_handle_t * handle) {
    pomelo_platform_executor_t * executor = handle->data;
    pomelo_platform_uv_executor_mgr_t * manager = executor->manager;
    pomelo_pool_release(manager->executor_pool, executor);

    assert(executor->entry != NULL);
    pomelo_list_remove(manager->executors, executor->entry);
    executor->entry = NULL;

    pomelo_platform_uv_executor_mgr_check_closed(manager);
}


int pomelo_platform_executor_shutdown(
    pomelo_platform_executor_t * executor,
    pomelo_platform_uv_executor_mgr_t * manager
) {
    assert(executor != NULL);
    assert(manager != NULL);

    if (executor->flags & POMELO_UV_EXECUTOR_FLAG_BUSY) {
        // Just set the flag for later shutdown
        executor->flags |= POMELO_UV_EXECUTOR_FLAG_SHUTDOWN;
        return 0; // Executor is busy
    }

    uv_mutex_t * mutex = &executor->mutex;

    /* ----------- Begin mutex scope ----------- */
    uv_mutex_lock(mutex);

    if (!pomelo_atomic_int64_compare_exchange(
        &executor->running, /* expected */ true, /* desired */ false
    )) {
        uv_mutex_unlock(mutex);
        assert(false);
        return -1; // Executor is not running
    }

    // Close the async handle under the lock so a concurrent submit either
    // observes running==true and sends before this, or observes running==false
    // and never touches the handle.
    uv_async_t * async = &executor->uv_async;
    uv_close((uv_handle_t *) async, executor_shutdown_complete);

    // Update task counter
    pomelo_atomic_uint64_fetch_sub(
        &manager->task_counter,
        executor->tasks_front->size + executor->tasks_back->size
    );

    pomelo_platform_uv_task_executor_t * task = NULL;
    pomelo_list_t * tasks = executor->tasks_front;
    while (pomelo_list_pop_front(tasks, &task) == 0) {
        pomelo_platform_uv_task_executor_release(task);
    }

    tasks = executor->tasks_back;
    while (pomelo_list_pop_front(tasks, &task) == 0) {
        pomelo_platform_uv_task_executor_release(task);
    }

    uv_mutex_unlock(mutex);
    /* ------------ End mutex scope ------------ */

    return 0;
}
