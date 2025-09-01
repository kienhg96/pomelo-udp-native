#ifndef POMELO_PLATFORM_H
#define POMELO_PLATFORM_H
#include <stdint.h>
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif

/// @brief Platform interface for managing network and system resources.
/// 
/// The platform layer provides core functionality for:
/// - Network socket management and I/O
/// - Task scheduling and execution
/// - Timer management
/// - System time access
///
/// To safely execute code from other threads, use the threadsafe executor
/// APIs like `pomelo_platform_threadsafe_executor_submit()` to submit tasks
/// that will run on the main platform thread.
typedef struct pomelo_platform_s pomelo_platform_t;

/// @brief The threadsafe executor interface
///
/// This interface is used to submit tasks to the platform thread.
/// It is threadsafe and can be called from any thread.
/// 
/// The tasks will be executed in the platform thread.
typedef struct pomelo_platform_uv_executor_s pomelo_platform_executor_t;

/// @brief Handle for executor, timer, udp and tasks
typedef struct pomelo_platform_handle_s pomelo_platform_handle_t;

/// @brief The entry point of task
typedef void (*pomelo_platform_task_entry)(void * data);

/// @brief The callback function of platform
typedef void (*pomelo_platform_callback)(pomelo_platform_t * platform);


struct pomelo_platform_handle_s {
    /// @brief The handle type
    int type;

    /// @brief The handle data
    void * data;
};


/* -------------------------------------------------------------------------- */
/*                               Common APIs                                  */
/* -------------------------------------------------------------------------- */


/// @brief Set the platform extra data
void pomelo_platform_set_extra(pomelo_platform_t * platform, void * data);


/// @brief Get the platform extra data
void * pomelo_platform_get_extra(pomelo_platform_t * platform);


/// @brief Close the platform.
/// All resources will be closed, then the callback will be called.
/// @return 0 if close task is submitted or -1 if platform is still closing.
int pomelo_platform_close(
    pomelo_platform_t * platform,
    pomelo_platform_callback callback
);


/* -------------------------------------------------------------------------- */
/*                                 Time APIs                                  */
/* -------------------------------------------------------------------------- */

/// @brief Get the high-resolution time (nanoseconds) (Threadsafe)
uint64_t pomelo_platform_hrtime(pomelo_platform_t * platform);


/// @brief Get the time in unix timestamp (milliseconds) (Threadsafe)
uint64_t pomelo_platform_now(pomelo_platform_t * platform);


/* -------------------------------------------------------------------------- */
/*                           Threadsafe executor APIs                         */
/* -------------------------------------------------------------------------- */

/// @brief Acquire an executor (Non-threadsafe).
/// @param platform The platform
/// @param executor_handle The handle of executor
/// @return 0 if acquire task is submitted or -1 if failed
int pomelo_platform_acquire_executor(
    pomelo_platform_t * platform,
    pomelo_platform_handle_t * executor_handle
);


/// @brief Release the executor (Threadsafe).
/// @param platform The platform
/// @param executor_handle The handle of executor
void pomelo_platform_release_executor(
    pomelo_platform_t * platform,
    pomelo_platform_handle_t * executor_handle
);


/// @brief Submit a task to run in platform thread (Threadsafe).
/// If the platform is closing, the pending tasks will not be executed.
/// In this case, the resources need to be cleaned up mannually.
/// @param platform The platform
/// @param executor_handle The handle of executor
/// @param entry The entry point of task
/// @param data The data to pass to the entry point
/// @return 0 if task is submitted or -1 if failed
int pomelo_platform_executor_submit(
    pomelo_platform_t * platform,
    pomelo_platform_handle_t * executor_handle,
    pomelo_platform_task_entry entry,
    void * data
);


#ifdef __cplusplus
}
#endif
#endif // POMELO_PLATFORM_H
