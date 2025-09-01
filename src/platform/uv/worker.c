#include <assert.h>
#include <string.h>
#include "worker.h"


/* -------------------------------------------------------------------------- */
/*                                Public APIs                                 */
/* -------------------------------------------------------------------------- */

pomelo_platform_uv_worker_mgr_t * pomelo_platform_uv_worker_mgr_create(
    pomelo_platform_uv_t * platform,
    pomelo_allocator_t * allocator,
    uv_loop_t * uv_loop
) {
    assert(allocator != NULL);
    assert(uv_loop != NULL);

    pomelo_platform_uv_worker_mgr_t * manager = pomelo_allocator_malloc_t(
        allocator,
        pomelo_platform_uv_worker_mgr_t
    );
    if (!manager) return NULL;
    memset(manager, 0, sizeof(pomelo_platform_uv_worker_mgr_t));
    manager->platform = platform;
    manager->allocator = allocator;
    manager->uv_loop = uv_loop;

    pomelo_pool_root_options_t pool_options = {
        .allocator = allocator,
        .element_size = sizeof(pomelo_platform_uv_task_worker_t),
        .zero_init = true
    };
    manager->task_pool = pomelo_pool_root_create(&pool_options);
    if (!manager->task_pool) {
        pomelo_platform_uv_worker_mgr_destroy(manager);
        return NULL;
    }

    pomelo_list_options_t list_options = {
        .allocator = allocator,
        .element_size = sizeof(pomelo_platform_uv_task_worker_t *)
    };
    manager->tasks = pomelo_list_create(&list_options);
    if (!manager->tasks) {
        pomelo_platform_uv_worker_mgr_destroy(manager);
        return NULL;
    }

    return manager;
}


void pomelo_platform_uv_worker_mgr_destroy(
    pomelo_platform_uv_worker_mgr_t * manager
) {
    assert(manager != NULL);
    pomelo_allocator_t * allocator = manager->allocator;
    if (manager->task_pool) {
        pomelo_pool_destroy(manager->task_pool);
        manager->task_pool = NULL;
    }

    if (manager->tasks) {
        pomelo_list_destroy(manager->tasks);
        manager->tasks = NULL;
    }

    pomelo_allocator_free(allocator, manager);
}


void pomelo_platform_uv_worker_mgr_close(
    pomelo_platform_uv_worker_mgr_t * manager
) {
    assert(manager != NULL);
    pomelo_platform_uv_task_worker_t * task = NULL;
    pomelo_list_iterator_t it;
    pomelo_list_iterator_init(&it, manager->tasks);
    while (pomelo_list_iterator_next(&it, &task) == 0) {
        pomelo_platform_uv_worker_task_cancel_ex(task);
    }

    pomelo_platform_uv_worker_mgr_check_closed(manager);
}


void pomelo_platform_uv_worker_mgr_statistic(
    pomelo_platform_uv_worker_mgr_t * manager,
    pomelo_statistic_platform_uv_t * statistic
) {
    assert(manager != NULL);
    assert(statistic != NULL);
    statistic->worker_tasks = manager->tasks->size;
}


int pomelo_platform_uv_submit_worker_task(
    pomelo_platform_uv_t * platform,
    pomelo_platform_task_entry entry,
    pomelo_platform_task_complete complete,
    void * data,
    pomelo_platform_handle_t * task_handle
) {
    assert(platform != NULL);
    assert(entry != NULL);

    pomelo_platform_uv_worker_mgr_t * manager = platform->worker_mgr;
    pomelo_platform_uv_task_worker_t * task =
        pomelo_pool_acquire(manager->task_pool, NULL);
    if (!task) return -1; // Failed to acquire new task

    task->manager = manager;
    task->entry = entry;
    task->complete = complete;
    task->data = data;

    task->list_entry = pomelo_list_push_back(manager->tasks, task);
    if (!task->list_entry) {
        pomelo_platform_uv_worker_release(task);
        return -1; // Failed to append to global list
    }

    // Setup UV work
    uv_work_t * work = &task->uv_work;
    work->data = task;

    int ret = uv_queue_work(
        manager->uv_loop,
        work,
        pomelo_platform_uv_worker_entry,
        pomelo_platform_uv_worker_done
    );
    if (ret < 0) {
        pomelo_platform_uv_worker_release(task);
        return -1; // Failed to queue work
    }

    task->handle = task_handle;
    if (task_handle) {
        task_handle->type = POMELO_PLATFORM_HANDLE_TASK_WORKER;
        task_handle->data = task;
    }

    return 0;
}


void pomelo_platform_uv_cancel_worker_task(
    pomelo_platform_uv_t * platform,
    pomelo_platform_handle_t * task_handle
) {
    (void) platform;
    assert(task_handle != NULL);

    if (task_handle->type != POMELO_PLATFORM_HANDLE_TASK_WORKER) return;
    pomelo_platform_uv_task_worker_t * task = task_handle->data;
    assert(task != NULL);

    pomelo_platform_uv_worker_task_cancel_ex(task);

    // Detach the handle
    task_handle->type = POMELO_PLATFORM_HANDLE_NONE;
    task_handle->data = NULL;
    task->handle = NULL;
}


/* -------------------------------------------------------------------------- */
/*                               Private APIs                                 */
/* -------------------------------------------------------------------------- */


void pomelo_platform_uv_worker_mgr_check_closed(
    pomelo_platform_uv_worker_mgr_t * manager
) {
    assert(manager != NULL);
    if (manager->tasks->size == 0) {
        pomelo_platform_uv_worker_mgr_on_closed(manager);
        return;
    }
}


void pomelo_platform_uv_worker_entry(uv_work_t * uv_work) {
    pomelo_platform_uv_task_worker_t * task = uv_work->data;
    task->entry(task->data);
}


void pomelo_platform_uv_worker_done(uv_work_t * uv_work, int status) {
    pomelo_platform_uv_task_worker_t * task = uv_work->data;
    pomelo_platform_uv_worker_mgr_t * controller = task->manager;
    bool canceled = (status == UV_ECANCELED || task->canceled);
    pomelo_platform_task_complete complete = task->complete;
    void * data = task->data;

    // Detach the handle
    pomelo_platform_handle_t * handle = task->handle;
    if (handle &&
        handle->type == POMELO_PLATFORM_HANDLE_TASK_WORKER &&
        handle->data == task
    ) {
        handle->type = POMELO_PLATFORM_HANDLE_NONE;
        handle->data = NULL;
    }
    task->handle = NULL;

    pomelo_platform_uv_worker_release(task);

    // Worker tasks eventually will be done.
    complete(data, canceled ? POMELO_PLATFORM_TASK_FLAG_CANCELED : 0);
    pomelo_platform_uv_worker_mgr_check_closed(controller);
}


void pomelo_platform_uv_worker_release(
    pomelo_platform_uv_task_worker_t * task
) {
    assert(task != NULL);

    pomelo_platform_uv_worker_mgr_t * controller = task->manager;
    pomelo_list_remove(controller->tasks, task->list_entry);
    pomelo_pool_release(controller->task_pool, task);
}


void pomelo_platform_uv_worker_task_cancel_ex(
    pomelo_platform_uv_task_worker_t * task
) {
    assert(task != NULL);
    if (task->canceled) return;
    
    task->canceled = true;
    uv_cancel((uv_req_t *) &task->uv_work);
}
