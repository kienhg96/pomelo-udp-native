#ifndef POMELO_PLATFORM_UV_EXECUTOR_SRC_H
#define POMELO_PLATFORM_UV_EXECUTOR_SRC_H
#include "utils/list.h"
#include "utils/atomic.h"
#include "utils/pool.h"
#include "platform-uv.h"

#ifdef __cplusplus
extern "C" {
#endif


/// @brief Busy flag of executor
#define POMELO_UV_EXECUTOR_FLAG_BUSY       (1 << 0)

/// @brief Shutdown flag of executor
#define POMELO_UV_EXECUTOR_FLAG_SHUTDOWN   (1 << 1)


/// @brief Threadsafe task
typedef struct pomelo_platform_uv_task_executor_s
    pomelo_platform_uv_task_executor_t;


struct pomelo_platform_uv_executor_mgr_s {
    /// @brief Platform
    pomelo_platform_uv_t * platform;

    /// @brief Allocator
    pomelo_allocator_t * allocator;

    /// @brief UV loop
    uv_loop_t * uv_loop;

    /// @brief Pool of tasks
    pomelo_pool_t * task_pool;

    /// @brief Pool of executors
    pomelo_pool_t * executor_pool;

    /// @brief Counter of tasks
    pomelo_atomic_uint64_t task_counter;

    /// @brief List of executors
    pomelo_list_t * executors;
};


struct pomelo_platform_uv_executor_s {
    /// @brief Manager
    pomelo_platform_uv_executor_mgr_t * manager;

    /// @brief Running flag of executor
    pomelo_atomic_int64_t running;

    /// @brief Front tasks list for queuing
    pomelo_list_t * tasks_front;

    /// @brief Back tasks list for executing
    pomelo_list_t * tasks_back;

    /// @brief Mutex of controller
    uv_mutex_t mutex;

    /// @brief UV async
    uv_async_t uv_async;

    /// @brief Flags of executor
    uint32_t flags;

    /// @brief Entry of this executor
    pomelo_list_entry_t * entry;

    /// @brief Handle of this executor
    pomelo_platform_handle_t * handle;
};


struct pomelo_platform_uv_task_executor_s {
    /// @brief Controller
    pomelo_platform_uv_executor_mgr_t * manager;

    /// @brief Entry of this task
    pomelo_platform_task_entry entry;

    /// @brief Data of this task
    void * data;
};


/* -------------------------------------------------------------------------- */
/*                                Public APIs                                 */
/* -------------------------------------------------------------------------- */

/// @brief Create executor controller
pomelo_platform_uv_executor_mgr_t * pomelo_platform_uv_executor_mgr_create(
    pomelo_platform_uv_t * platform,
    pomelo_allocator_t * allocator,
    uv_loop_t * uv_loop
);


/// @brief Destroy executor controller
void pomelo_platform_uv_executor_mgr_destroy(
    pomelo_platform_uv_executor_mgr_t * manager
);


/// @brief Close the executor controller
void pomelo_platform_uv_executor_mgr_close(
    pomelo_platform_uv_executor_mgr_t * manager
);


/// @brief Callback when the executor controller is completely closed
void pomelo_platform_uv_executor_mgr_on_closed(
    pomelo_platform_uv_executor_mgr_t * manager
);


/// @brief Get statistic information
void pomelo_platform_uv_executor_mgr_statistic(
    pomelo_platform_uv_executor_mgr_t * manager,
    pomelo_statistic_platform_uv_t * statistic
);


/* -------------------------------------------------------------------------- */
/*                               Private APIs                                 */
/* -------------------------------------------------------------------------- */

/// @brief Check close
void pomelo_platform_uv_executor_mgr_check_closed(
    pomelo_platform_uv_executor_mgr_t * manager
);


/// @brief Release executor task
void pomelo_platform_uv_task_executor_release(
    pomelo_platform_uv_task_executor_t * task
);


/// @brief Async callback
void pomelo_platform_task_executor_async_callback(uv_async_t * async);


/// @brief Initialize the platform executor
int pomelo_platform_executor_on_alloc(
    pomelo_platform_executor_t * executor,
    pomelo_platform_uv_executor_mgr_t * manager
);


/// @brief Finalize the platform executor
void pomelo_platform_executor_on_free(pomelo_platform_executor_t * executor);


/// @brief Startup the platform executor
int pomelo_platform_executor_startup(
    pomelo_platform_executor_t * executor,
    pomelo_platform_uv_executor_mgr_t * manager
);


/// @brief Shutdown the platform executor
int pomelo_platform_executor_shutdown(
    pomelo_platform_executor_t * executor,
    pomelo_platform_uv_executor_mgr_t * manager
);


#ifdef __cplusplus
}
#endif
#endif // POMELO_PLATFORM_UV_EXECUTOR_SRC_H
