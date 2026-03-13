#ifndef POMELO_PLATFORM_UV_TASK_WORKER_SRC_H
#define POMELO_PLATFORM_UV_TASK_WORKER_SRC_H
#include "utils/pool.h"
#include "utils/list.h"
#include "platform-uv.h"

#ifdef __cplusplus
extern "C" {
#endif


/// @brief Worker task
typedef struct pomelo_platform_uv_task_worker_s
    pomelo_platform_uv_task_worker_t;


struct pomelo_platform_uv_worker_mgr_s {
    /// @brief Platform
    pomelo_platform_uv_t * platform;

    /// @brief Allocator
    pomelo_allocator_t * allocator;

    /// @brief UV Loop
    uv_loop_t * uv_loop;

    /// @brief Pool of worker tasks
    pomelo_pool_t * task_pool;

    /// @brief Processing tasks
    pomelo_list_t * tasks;
};


struct pomelo_platform_uv_task_worker_s {
    /// @brief Manager
    pomelo_platform_uv_worker_mgr_t * manager;

    /// @brief The entry point of task
    pomelo_platform_task_entry entry;

    /// @brief The done point of task
    pomelo_platform_task_complete complete;

    /// @brief Callback data
    void * data;

    /// @brief Canceled flag
    bool canceled;

    /// @brief UV work
    uv_work_t uv_work;

    /// @brief Entry of this task in the list of controller
    pomelo_list_entry_t * list_entry;

    /// @brief The handle of worker
    pomelo_platform_handle_t * handle;
};


/* -------------------------------------------------------------------------- */
/*                                Public APIs                                 */
/* -------------------------------------------------------------------------- */


/// @brief Create task worker manager
pomelo_platform_uv_worker_mgr_t * pomelo_platform_uv_worker_mgr_create(
    pomelo_platform_uv_t * platform,
    pomelo_allocator_t * allocator,
    uv_loop_t * uv_loop
);


/// @brief Destroy task worker manager
void pomelo_platform_uv_worker_mgr_destroy(
    pomelo_platform_uv_worker_mgr_t * manager
);


/// @brief Close the manager
void pomelo_platform_uv_worker_mgr_close(
    pomelo_platform_uv_worker_mgr_t * manager
);


/// @brief Callback when the manager is completely closed
void pomelo_platform_uv_worker_mgr_on_closed(
    pomelo_platform_uv_worker_mgr_t * manager
);


/// @brief Get statistic information
void pomelo_platform_uv_worker_mgr_statistic(
    pomelo_platform_uv_worker_mgr_t * manager,
    pomelo_statistic_platform_uv_t * statistic
);


/* -------------------------------------------------------------------------- */
/*                               Private APIs                                 */
/* -------------------------------------------------------------------------- */


/// @brief Check if the manager is closed
void pomelo_platform_uv_worker_mgr_check_closed(
    pomelo_platform_uv_worker_mgr_t * manager
);


/// @brief The entry point of worker task
void pomelo_platform_uv_worker_entry(uv_work_t * uv_work);


/// @brief The done point of worker task
void pomelo_platform_uv_worker_done(uv_work_t * uv_work, int status);


/// @brief Release task worker
void pomelo_platform_uv_worker_release(pomelo_platform_uv_task_worker_t * task);


/// @brief Cancel worker task
void pomelo_platform_uv_worker_task_cancel_ex(
    pomelo_platform_uv_task_worker_t * task
);


#ifdef __cplusplus
}
#endif
#endif // POMELO_PLATFORM_UV_TASK_WORKER_SRC_H
