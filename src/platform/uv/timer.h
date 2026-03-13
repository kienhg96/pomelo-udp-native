#ifndef POMELO_PLATFORM_UV_TIMER_SRC_H
#define POMELO_PLATFORM_UV_TIMER_SRC_H
#include "utils/pool.h"
#include "utils/list.h"
#include "platform-uv.h"

#ifdef __cplusplus
extern "C" {
#endif


/// @brief Platform UV timer
typedef struct pomelo_platform_uv_timer_s pomelo_platform_uv_timer_t;


struct pomelo_platform_uv_timer_s {
    /// @brief The user data
    void * data;

    /// @brief The manager
    pomelo_platform_uv_timer_mgr_t * manager;

    /// @brief The callback
    pomelo_platform_timer_entry entry;

    /// @brief The timer
    uv_timer_t uv_timer;

    /// @brief The repeat flag
    bool is_repeat;

    /// @brief The running flag
    bool is_running;

    /// @brief The entry of this timer in the controlled list of the manager
    pomelo_list_entry_t * list_entry;

    /// @brief The handle of this timer
    pomelo_platform_handle_t * handle;
};


struct pomelo_platform_uv_timer_mgr_s {
    /// @brief Platform
    pomelo_platform_uv_t * platform;

    /// @brief Allocator
    pomelo_allocator_t * allocator;

    /// @brief UV loop
    uv_loop_t * uv_loop;

    /// @brief The timer pool
    pomelo_pool_t * timer_pool;

    /// @brief All active timers
    pomelo_list_t * timers;
};


/* -------------------------------------------------------------------------- */
/*                                Public APIs                                 */
/* -------------------------------------------------------------------------- */


/// @brief Create the timer manager
pomelo_platform_uv_timer_mgr_t * pomelo_platform_uv_timer_mgr_create(
    pomelo_platform_uv_t * platform,
    pomelo_allocator_t * allocator,
    uv_loop_t * uv_loop
);


/// @brief Destroy the timer manager
void pomelo_platform_uv_timer_mgr_destroy(
    pomelo_platform_uv_timer_mgr_t * manager
);


/// @brief Get the statistic of timer manager
void pomelo_platform_uv_timer_mgr_statistic(
    pomelo_platform_uv_timer_mgr_t * manager,
    pomelo_statistic_platform_uv_t * statistic
);


/// @brief Close the timer manager
void pomelo_platform_uv_timer_mgr_close(
    pomelo_platform_uv_timer_mgr_t * manager
);


/// @brief Callback when the timer manager is completely closed
void pomelo_platform_uv_timer_mgr_on_closed(
    pomelo_platform_uv_timer_mgr_t * manager
);


/* -------------------------------------------------------------------------- */
/*                               Private APIs                                 */
/* -------------------------------------------------------------------------- */


/// @brief Check close
void pomelo_platform_uv_timer_mgr_check_closed(
    pomelo_platform_uv_timer_mgr_t * manager
);


/// @brief Stop timer
int pomelo_platform_uv_timer_stop_ex(pomelo_platform_uv_timer_t * timer);


/// @brief The callback from uv timer
void pomelo_platform_uv_timer_callback(uv_timer_t * uv_timer);


#ifdef __cplusplus
}
#endif
#endif // POMELO_PLATFORM_UV_TIMER_SRC_H
