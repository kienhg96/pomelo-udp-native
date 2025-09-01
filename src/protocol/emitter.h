#ifndef POMELO_PROTOCOL_EMITTER_SRC_H
#define POMELO_PROTOCOL_EMITTER_SRC_H
#include <stdbool.h>
#include <stdint.h>
#include "protocol/socket.h"
#ifdef __cplusplus
extern "C" {
#endif


/// @brief The frequent emitter
typedef struct pomelo_protocol_emitter_s pomelo_protocol_emitter_t;


/// @brief The callback when the emitter has triggered
typedef void (*pomelo_emitter_cb)(pomelo_protocol_client_t * client);


/// @brief The options of emitter
typedef struct pomelo_protocol_emitter_options_s
    pomelo_protocol_emitter_options_t;


struct pomelo_protocol_emitter_options_s {
    /// @brief The client
    pomelo_protocol_client_t * client;

    /// @brief The frequency of emitter in Hz
    uint64_t frequency;

    /// @brief The limit of emitter.
    /// If zero is set, this will run forever.
    uint32_t limit;

    /// @brief The timeout of emitter.
    /// If zero is set, the emitter will have no timeout.
    uint64_t timeout_ms;

    /// @brief The callback when the emitter is triggered
    pomelo_emitter_cb trigger_cb;

    /// @brief The callback when the emitter has timed out
    pomelo_emitter_cb timeout_cb;

    /// @brief The callback when the counter has reached limit
    pomelo_emitter_cb limit_cb;
};


struct pomelo_protocol_emitter_s {
    /// @brief The client
    pomelo_protocol_client_t * client;

    /// @brief The frequency of emitter in Hz
    uint64_t frequency;

    /// @brief The limit of emitter.
    /// If zero is set, this will run forever.
    uint32_t limit;

    /// @brief The timeout of emitter.
    /// If zero is set, the emitter will have no timeout.
    uint64_t timeout_ms;

    /// @brief The callback when the emitter is triggered
    pomelo_emitter_cb trigger_cb;

    /// @brief The callback when the emitter has timed out
    pomelo_emitter_cb timeout_cb;

    /// @brief The callback when the counter has reached limit
    pomelo_emitter_cb limit_cb;

    /// @brief Running flag of emitter
    bool running;

    /// @brief The counter of triggered times.
    /// This will be reset right after starting
    uint32_t trigger_counter;

    /// @brief The triggering timer
    pomelo_platform_handle_t trigger_timer;

    /// @brief The timer of timing out
    pomelo_platform_handle_t timeout_timer;

    /// @brief The task of triggering
    pomelo_sequencer_task_t trigger_task;

    /// @brief The task of timing out
    pomelo_sequencer_task_t timeout_task;
};


/* -------------------------------------------------------------------------- */
/*                                Public APIs                                 */
/* -------------------------------------------------------------------------- */

/// @brief Create new frequent emitter
int pomelo_protocol_emitter_init(
    pomelo_protocol_emitter_t * emitter,
    pomelo_protocol_emitter_options_t * options
);


/// @brief Start the frequent emitter.
/// Calling start without initializing will lead to undefined behavior
/// @return Returns 0 on success, or an error code < 0 on failure
int pomelo_protocol_emitter_start(pomelo_protocol_emitter_t * emitter);


/// @brief Stop the frequent emitter
void pomelo_protocol_emitter_stop(pomelo_protocol_emitter_t * emitter);


/* -------------------------------------------------------------------------- */
/*                                Private APIs                                */
/* -------------------------------------------------------------------------- */

/// @brief The callback when the emitter has triggered
void pomelo_protocol_emitter_on_triggered(pomelo_protocol_emitter_t * emitter);


/// @brief The callback when the emitter has timed out
void pomelo_protocol_emitter_on_timeout(pomelo_protocol_emitter_t * emitter);


#ifdef __cplusplus
}
#endif
#endif // POMELO_PROTOCOL_EMITTER_SRC_H
