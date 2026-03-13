#ifndef POMELO_PLATFORM_SRC_H
#define POMELO_PLATFORM_SRC_H
#include <stdbool.h>
#include "pomelo/allocator.h"
#include "pomelo/address.h"
#include "pomelo/platform.h"


#ifdef __cplusplus
extern "C" {
#endif


/// @brief Task has been canceled
#define POMELO_PLATFORM_TASK_FLAG_CANCELED (1 << 0)


/// @brief The buffer vector for sending
typedef struct pomelo_platform_iovec_s pomelo_platform_iovec_t;

/// @brief The callback after task is done
typedef void (*pomelo_platform_task_complete)(void * data, uint32_t flags);

/// @brief The payload receiving callback
typedef void (*pomelo_platform_recv_cb)(
    void * context,
    pomelo_address_t * address,
    pomelo_platform_iovec_t * iovec,
    int status
);

/// @brief The payload sending callback
typedef void (*pomelo_platform_send_cb)(void * callback_data, int status);

/// @brief The payload allocation callback for platform
typedef void (*pomelo_platform_alloc_cb)(
    void * context,
    pomelo_platform_iovec_t * iovec
);

/// @brief The timer entry
typedef void (*pomelo_platform_timer_entry)(void * data);

struct pomelo_platform_iovec_s {
    /// @brief The data pointer
    uint8_t * data;

    /// @brief The length of pointer
    size_t length;
};


typedef enum pomelo_platform_handle_type {
    POMELO_PLATFORM_HANDLE_NONE = 0, // Empty handle
    POMELO_PLATFORM_HANDLE_EXECUTOR,
    POMELO_PLATFORM_HANDLE_TIMER,
    POMELO_PLATFORM_HANDLE_UDP,
    POMELO_PLATFORM_HANDLE_TASK_WORKER,
    POMELO_PLATFORM_HANDLE_TASK_EXECUTOR,
} pomelo_platform_handle_type;


/* -------------------------------------------------------------------------- */
/*                            Platform Task APIs                              */
/* -------------------------------------------------------------------------- */


/// @brief Submit a work to run in threadpool with group
int pomelo_platform_submit_worker_task(
    pomelo_platform_t * platform,
    pomelo_platform_task_entry entry,
    pomelo_platform_task_complete complete,
    void * data,
    pomelo_platform_handle_t * task_handle
);


/// @brief Cancel the worker task
void pomelo_platform_cancel_worker_task(
    pomelo_platform_t * platform,
    pomelo_platform_handle_t * task_handle
);


/* -------------------------------------------------------------------------- */
/*                             Platform UDP APIs                              */
/* -------------------------------------------------------------------------- */

/// @brief Bind the socket with specific address
int pomelo_platform_udp_bind(
    pomelo_platform_t * platform,
    pomelo_address_t * address,
    pomelo_platform_handle_t * udp_handle
);


/// @brief Connect the socket to specific address
int pomelo_platform_udp_connect(
    pomelo_platform_t * platform,
    pomelo_address_t * address,
    pomelo_platform_handle_t * udp_handle
);


/// @brief Stop the socket UDP
void pomelo_platform_udp_stop(
    pomelo_platform_t * platform,
    pomelo_platform_handle_t * udp_handle
);


/// @brief Send a packet to target
/// @param callback_data The data which will be passed to send_callback
int pomelo_platform_udp_send(
    pomelo_platform_t * platform,
    pomelo_platform_handle_t * udp_handle,
    pomelo_address_t * address,
    int nbuffers,
    pomelo_platform_iovec_t * buffers,
    void * callback_data,
    pomelo_platform_send_cb send_callback
);


/// @brief Start receiving packets from socket
void pomelo_platform_udp_recv_start(
    pomelo_platform_t * platform,
    pomelo_platform_handle_t * udp_handle,
    void * context,
    pomelo_platform_alloc_cb alloc_callback,
    pomelo_platform_recv_cb recv_callback
);


/* -------------------------------------------------------------------------- */
/*                            Platform Timer APIs                             */
/* -------------------------------------------------------------------------- */

/// @brief Start the timer.
/// @return 0 on success or an error code < 0 on failure
int pomelo_platform_timer_start(
    pomelo_platform_t * platform,
    pomelo_platform_timer_entry entry,
    uint64_t timeout_ms,
    uint64_t repeat_ms,
    void * data,
    pomelo_platform_handle_t * timer_handle
);


/// @brief Stop the timer
void pomelo_platform_timer_stop(
    pomelo_platform_t * platform,
    pomelo_platform_handle_t * timer_handle
);


#ifdef __cplusplus
}
#endif
#endif // POMELO_PLATFORM_SRC_H
