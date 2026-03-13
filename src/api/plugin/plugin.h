#ifndef POMELO_API_PLUGIN_SRC_H
#define POMELO_API_PLUGIN_SRC_H
#include "pomelo/api.h"
#include "pomelo/plugin.h"
#include "api/session.h"
#include "utils/map.h"
#include "utils/atomic.h"


#ifndef NDEBUG
#define POMELO_PLUGIN_SIGNATURE 0xfa12e7
#define pomelo_plugin_check_signature(plugin)                                  \
    assert((plugin)->signature == POMELO_PLUGIN_SIGNATURE)
#else
#define pomelo_plugin_check_signature(plugin) (void) (plugin)
#endif

#ifdef __cplusplus
extern "C" {
#endif


#define POMELO_PLUGIN_FLAG_EXECUTOR_STARTED (1 << 0)


/// @brief The plugin manager
typedef struct pomelo_plugin_manager_s pomelo_plugin_manager_t;

/// @brief The session for plugins
typedef struct pomelo_session_plugin_s pomelo_session_plugin_t;

/// @brief The associated channel with plugin sessions
typedef struct pomelo_channel_plugin_s pomelo_channel_plugin_t;

/// @brief The implementation of plugin enviroment
typedef struct pomelo_plugin_impl_s pomelo_plugin_impl_t;


struct pomelo_plugin_impl_s {
    /// @brief Base plugin
    pomelo_plugin_t base;

    /// @brief The allocator
    pomelo_allocator_t * allocator;

    /// @brief The context of plugin
    pomelo_context_t * context;

    /// @brief The platform of plugin
    pomelo_platform_t * platform;

    /// @brief Associated data
    pomelo_atomic_uint64_t data;

    /// @brief Entry of this plugin in attached list
    pomelo_list_entry_t * attached_entry;

    /// @brief Acquired messages for later releasing
    pomelo_list_t * acquired_messages;

    /// @brief Thread-safe executor for this plugin
    pomelo_platform_handle_t executor;

    /// @brief [Synchronized] Pool of commands for the thread-safe executor
    pomelo_pool_t * command_pool;

    /// @brief The flags of this plugin
    uint32_t flags;

    /* Callbacks */
    pomelo_plugin_on_unload_callback on_unload_callback;
    pomelo_plugin_socket_common_callback socket_on_created_callback;
    pomelo_plugin_socket_common_callback socket_on_destroyed_callback;
    pomelo_plugin_socket_listening_callback socket_on_listening_callback;
    pomelo_plugin_socket_connecting_callback socket_on_connecting_callback;
    pomelo_plugin_socket_common_callback socket_on_stopped_callback;
    pomelo_plugin_session_send_callback session_on_send_callback;
    pomelo_plugin_session_disconnect_callback session_disconnect_callback;
    pomelo_plugin_session_get_rtt_callback session_get_rtt_callback;
    pomelo_plugin_session_set_mode_callback session_set_channel_mode_callback;

#ifndef NDEBUG
    /// @brief The signature of the plugin
    int signature;
#endif
};


/* -------------------------------------------------------------------------- */
/*                                Public APIs                                 */
/* -------------------------------------------------------------------------- */

/// @brief Stop the socket for all plugins
void pomelo_plugin_stop_socket(pomelo_socket_t * socket);


/* -------------------------------------------------------------------------- */
/*                                Private APIs                                */
/* -------------------------------------------------------------------------- */

/// @brief Create new plugin
pomelo_plugin_impl_t * pomelo_plugin_create(
    pomelo_allocator_t * allocator,
    pomelo_context_t * context,
    pomelo_platform_t * platform
);


/// @brief Destroy the plugin
void pomelo_plugin_destroy(pomelo_plugin_impl_t * plugin);


/* -------------------------------------------------------------------------- */
/*                               Exported APIs                                */
/* -------------------------------------------------------------------------- */

/// @brief Configure the plugin
void POMELO_PLUGIN_CALL pomelo_plugin_configure(
    pomelo_plugin_t * plugin,
    pomelo_plugin_on_unload_callback on_unload_callback,
    pomelo_plugin_socket_common_callback socket_on_created_callback,
    pomelo_plugin_socket_common_callback socket_on_destroyed_callback,
    pomelo_plugin_socket_listening_callback socket_on_listening_callback,
    pomelo_plugin_socket_connecting_callback socket_on_connecting_callback,
    pomelo_plugin_socket_common_callback socket_on_stopped_callback,
    pomelo_plugin_session_send_callback session_on_send_callback,
    pomelo_plugin_session_disconnect_callback session_disconnect_callback,
    pomelo_plugin_session_get_rtt_callback session_get_rtt_callback,
    pomelo_plugin_session_set_mode_callback session_set_mode_callback
);

/// @brief Set associated data for plugin
void POMELO_PLUGIN_CALL pomelo_plugin_set_data(
    pomelo_plugin_t * plugin,
    void * data
);

/// @brief Get associated data from plugin
void * POMELO_PLUGIN_CALL pomelo_plugin_get_data(pomelo_plugin_t * plugin);

/* -------------------------------------------------------------------------- */
/*                        Internal dispatching APIs                           */
/* -------------------------------------------------------------------------- */

/// @brief Dispatch created event of socket
void pomelo_plugin_dispatch_socket_on_created(pomelo_socket_t * socket);

/// @brief Dispatch listening event of socket
void pomelo_plugin_dispatch_socket_on_listening(
    pomelo_socket_t * socket,
    pomelo_address_t * address
);

/// @brief Dispatch connecting event of socket
void pomelo_plugin_dispatch_socket_on_connecting(
    pomelo_socket_t * socket,
    const uint8_t * connect_token
);

/// @brief Dispatch stopped event of socket
void pomelo_plugin_dispatch_socket_on_stopped(pomelo_socket_t * socket);

/// @brief Dispatch destroyed event of socket
void pomelo_plugin_dispatch_socket_on_destroyed(pomelo_socket_t * socket);


/* -------------------------------------------------------------------------- */
/*                                Private APIs                                */
/* -------------------------------------------------------------------------- */

/// @brief Initialize the plugin template
void pomelo_plugin_init_template(pomelo_plugin_t * tpl);


/// @brief Parse address
void pomelo_plugin_parse_address(
    pomelo_address_t * address,
    pomelo_address_type address_type,
    uint8_t * address_host,
    uint16_t address_port
);


/// @brief Do cleanup after callbacks return
void pomelo_plugin_post_callback_cleanup(pomelo_plugin_impl_t * impl);


#ifdef __cplusplus
}
#endif
#endif // ~POMELO_API_PLUGIN_SRC_H
