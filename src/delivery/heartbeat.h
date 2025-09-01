#ifndef POMELO_DELIVERY_HEARTBEAT_SRC_H
#define POMELO_DELIVERY_HEARTBEAT_SRC_H
#include "utils/list.h"
#include "platform/platform.h"
#include "delivery.h"
#ifdef __cplusplus
extern "C" {
#endif


/// @brief Frequency of heartbeat
#define POMELO_DELIVERY_HEARTBEAT_FREQUENCY 10 // Hz


/// @brief Heartbeat handle
typedef struct pomelo_delivery_heartbeat_handle_s
    pomelo_delivery_heartbeat_handle_t;


struct pomelo_delivery_heartbeat_handle_s {
    /// @brief Entry of the endpoint in the list
    pomelo_list_entry_t * entry;
};


struct pomelo_delivery_heartbeat_s {
    /// @brief Context of heartbeat
    pomelo_delivery_context_t * context;

    /// @brief Platform of heartbeat
    pomelo_platform_t * platform;

    /// @brief All scheduled endpoints
    pomelo_list_t * endpoints;

    /// @brief Timer handle
    pomelo_platform_handle_t timer_handle;
};


/// @brief Alloc callback for heartbeat
int pomelo_delivery_heartbeat_on_alloc(
    pomelo_delivery_heartbeat_t * heartbeat,
    pomelo_delivery_context_t * context
);


/// @brief Free callback for heartbeat
void pomelo_delivery_heartbeat_on_free(pomelo_delivery_heartbeat_t * heartbeat);


/// @brief Initialize heartbeat
int pomelo_delivery_heartbeat_init(
    pomelo_delivery_heartbeat_t * heartbeat,
    pomelo_delivery_heartbeat_options_t * options
);


/// @brief Cleanup heartbeat
void pomelo_delivery_heartbeat_cleanup(pomelo_delivery_heartbeat_t * heartbeat);


/// @brief Schedule heartbeat for endpoints
int pomelo_delivery_heartbeat_schedule(
    pomelo_delivery_heartbeat_t * heartbeat,
    pomelo_delivery_endpoint_t * endpoint
);


/// @brief Unschedule heartbeat for endpoints
void pomelo_delivery_heartbeat_unschedule(
    pomelo_delivery_heartbeat_t * heartbeat,
    pomelo_delivery_endpoint_t * endpoint
);


/// @brief Heartbeat run
void pomelo_delivery_heartbeat_run(pomelo_delivery_heartbeat_t * heartbeat);


#ifdef __cplusplus
}
#endif
#endif // POMELO_DELIVERY_HEARTBEAT_SRC_H
