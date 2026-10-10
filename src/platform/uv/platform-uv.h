#ifndef POMELO_PLATFORM_UV_SRC_H
#define POMELO_PLATFORM_UV_SRC_H
#include "uv.h"
#include "pomelo/platforms/platform-uv.h"
#include "base/extra.h"
#include "platform/platform.h"

#ifdef __cplusplus
extern "C" {
#endif


#define POMELO_PLATFORM_UV_COMPONENT_TIMER      (1 << 0)
#define POMELO_PLATFORM_UV_COMPONENT_UDP        (1 << 1)
#define POMELO_PLATFORM_UV_COMPONENT_WORKER     (1 << 2)
#define POMELO_PLATFORM_UV_COMPONENT_THREADSAFE (1 << 3)
#define POMELO_PLATFORM_UV_COMPONENT_IDLE       (1 << 4)
#define POMELO_PLATFORM_UV_COMPONENT_ALL (    \
    POMELO_PLATFORM_UV_COMPONENT_TIMER      | \
    POMELO_PLATFORM_UV_COMPONENT_UDP        | \
    POMELO_PLATFORM_UV_COMPONENT_WORKER     | \
    POMELO_PLATFORM_UV_COMPONENT_THREADSAFE | \
    POMELO_PLATFORM_UV_COMPONENT_IDLE         \
)


#define POMELO_PLATFORM_UV_FLAG_CLOSING (1 << 0)


/// @brief Platform UV
typedef struct pomelo_platform_uv_s pomelo_platform_uv_t;

/// @brief The timer manager
typedef struct pomelo_platform_uv_timer_mgr_s pomelo_platform_uv_timer_mgr_t;

/// @brief The udp manager
typedef struct pomelo_platform_uv_udp_mgr_s pomelo_platform_uv_udp_mgr_t;

/// @brief The worker manager to run tasks in worker threads
typedef struct pomelo_platform_uv_worker_mgr_s pomelo_platform_uv_worker_mgr_t;

/// @brief The executor manager to run tasks in platform threads
typedef struct pomelo_platform_uv_executor_mgr_s
    pomelo_platform_uv_executor_mgr_t;


struct pomelo_platform_uv_s {
    /// @brief The extra data
    pomelo_extra_t extra;

    /// @brief The allocator
    pomelo_allocator_t * allocator;

    /// @brief The uv loop
    uv_loop_t * uv_loop;

    /// @brief The idle handle for shutdown
    uv_idle_t idle_close;

    /// @brief The flags
    uint32_t flags;

    /// @brief The closed callback
    pomelo_platform_callback closed_callback;

    /// @brief The bitmask of closed components
    uint32_t closed_components;

    /// @brief The timer manager
    pomelo_platform_uv_timer_mgr_t * timer_mgr;

    /// @brief The socket manager
    pomelo_platform_uv_udp_mgr_t * udp_mgr;

    /// @brief The worker manager to run tasks in worker threads
    pomelo_platform_uv_worker_mgr_t * worker_mgr;

    /// @brief The executor manager to run tasks in platform threads
    pomelo_platform_uv_executor_mgr_t * executor_mgr;
};

/* -------------------------------------------------------------------------- */
/*                               Public APIs                                  */
/* -------------------------------------------------------------------------- */

/// @brief Set the extra data of platform
void pomelo_platform_uv_set_extra(pomelo_platform_uv_t * platform, void * data);


/// @brief Get the extra data of platform
void * pomelo_platform_uv_get_extra(pomelo_platform_uv_t * platform);


/// @brief Close the platform
int pomelo_platform_uv_close(
    pomelo_platform_uv_t * platform,
    pomelo_platform_callback callback
);


/// @brief Acquire a threadsafe executor
int pomelo_platform_uv_acquire_executor(
    pomelo_platform_uv_t * platform,
    pomelo_platform_handle_t * executor_handle
);


/// @brief Release a threadsafe executor
void pomelo_platform_uv_release_executor(
    pomelo_platform_uv_t * platform,
    pomelo_platform_handle_t * executor_handle
);


/// @brief Submit a task to threadsafe executor
int pomelo_platform_uv_executor_submit(
    pomelo_platform_uv_t * platform,
    pomelo_platform_handle_t * executor_handle,
    pomelo_platform_task_entry entry,
    void * data
);


/// @brief Get high resolution time
uint64_t pomelo_platform_uv_hrtime(pomelo_platform_uv_t * platform);


/// @brief Get current time
uint64_t pomelo_platform_uv_now(pomelo_platform_uv_t * platform);


/// @brief Start a timer
int pomelo_platform_uv_timer_start(
    pomelo_platform_uv_t * platform,
    pomelo_platform_timer_entry entry,
    uint64_t timeout_ms,
    uint64_t repeat_ms,
    void * data,
    pomelo_platform_handle_t * timer_handle
);


/// @brief Stop a timer
void pomelo_platform_uv_timer_stop(
    pomelo_platform_uv_t * platform,
    pomelo_platform_handle_t * timer_handle
);


/// @brief Bind a UDP socket
int pomelo_platform_uv_udp_bind(
    pomelo_platform_uv_t * platform,
    pomelo_address_t * address,
    pomelo_platform_handle_t * udp_handle
);


/// @brief Connect a UDP socket
int pomelo_platform_uv_udp_connect(
    pomelo_platform_uv_t * platform,
    pomelo_address_t * address,
    pomelo_platform_handle_t * udp_handle
);


/// @brief Stop a UDP socket
void pomelo_platform_uv_udp_stop(
    pomelo_platform_uv_t * platform,
    pomelo_platform_handle_t * udp_handle
);


/// @brief Send data to a UDP socket
int pomelo_platform_uv_udp_send(
    pomelo_platform_uv_t * platform,
    pomelo_platform_handle_t * udp_handle,
    pomelo_address_t * address,
    int nbuffers,
    pomelo_platform_iovec_t * buffers,
    void * callback_data,
    pomelo_platform_send_cb send_callback
);


/// @brief Start receiving packets from socket
void pomelo_platform_uv_udp_recv_start(
    pomelo_platform_uv_t * platform,
    pomelo_platform_handle_t * udp_handle,
    void * context,
    pomelo_platform_alloc_cb alloc_callback,
    pomelo_platform_recv_cb recv_callback
);


/// @brief Submit a worker task
int pomelo_platform_uv_submit_worker_task(
    pomelo_platform_uv_t * platform,
    pomelo_platform_task_entry entry,
    pomelo_platform_task_complete complete,
    void * data,
    pomelo_platform_handle_t * task_handle
);


/// @brief Cancel a worker task
void pomelo_platform_uv_cancel_worker_task(
    pomelo_platform_uv_t * platform,
    pomelo_platform_handle_t * task_handle
);


/// @brief Drain all in-flight worker tasks (cancel + pump loop until none left)
void pomelo_platform_uv_drain_worker_tasks(pomelo_platform_uv_t * platform);


/* -------------------------------------------------------------------------- */
/*                              Private APIs                                  */
/* -------------------------------------------------------------------------- */

/// @brief Check if the platform is shutdown
void pomelo_platform_uv_check_closed(pomelo_platform_uv_t * platform);


#ifdef __cplusplus
}
#endif
#endif // POMELO_PLATFORM_UV_SRC_H
