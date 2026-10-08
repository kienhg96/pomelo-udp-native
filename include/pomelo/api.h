#ifndef POMELO_API_H
#define POMELO_API_H
#include "pomelo/common.h"
#include "pomelo/allocator.h"
#include "pomelo/address.h"
#include "pomelo/platform.h"
#include "pomelo/statistic.h"

/// @brief The Pomelo API provides a secure, connection-oriented networking
/// protocol for client-server game architectures.
///
/// Key features:
/// - Secure connection establishment using signed connect tokens
/// - Multiple delivery modes (unreliable, sequenced, reliable) via channels
/// - Connection quality statistics and monitoring
/// - IPv4 and IPv6 support
/// - Replay attack protection
/// - Optional multi-threading support
///
/// The protocol follows a client-server model where:
/// - Clients request connect tokens from a web backend
/// - Connect tokens contain encrypted credentials and server addresses
/// - Clients use tokens to establish secure connections with game servers
/// - Connected clients and servers exchange messages through configured
///   channels
///
/// Basic usage flow:
/// 1. Create a socket with desired channel configuration
/// 2. For servers: Start listening for connections
/// 3. For clients: Request connect token and connect to server
/// 4. Send/receive messages through channels
/// 5. Monitor connection quality via statistics
/// 6. Gracefully disconnect when done


#ifdef __cplusplus
extern "C" {
#endif


/// @brief Connect result
typedef enum pomelo_socket_connect_result {
    /// @brief The connection has timed out
    POMELO_SOCKET_CONNECT_TIMED_OUT = -2,

    /// @brief The connection has been denied
    POMELO_SOCKET_CONNECT_DENIED = -1,

    /// @brief The connection has been successfully established
    POMELO_SOCKET_CONNECT_SUCCESS = 0
} pomelo_socket_connect_result;


/// @brief Socket state
typedef enum pomelo_socket_state {
    /// @brief The socket has been stopped
    POMELO_SOCKET_STATE_STOPPED,

    /// @brief The socket is stopping
    POMELO_SOCKET_STATE_STOPPING,

    /// @brief The socket is running as a server
    POMELO_SOCKET_STATE_RUNNING_SERVER,

    /// @brief The socket is running as a client
    POMELO_SOCKET_STATE_RUNNING_CLIENT
} pomelo_socket_state;


/// @brief The options for creating socket
typedef struct pomelo_socket_options_s pomelo_socket_options_t;

/// @brief The api context creating options
typedef struct pomelo_context_root_options_s pomelo_context_root_options_t;

/// @brief The non-threadsafe API context creating options
typedef struct pomelo_context_shared_options_s pomelo_context_shared_options_t;

/// @brief Round trip time information
typedef struct pomelo_rtt_s pomelo_rtt_t;

/// @brief Session iterator. Do not modify iterator internal values manually.
typedef struct pomelo_session_iterator_s pomelo_session_iterator_t;


struct pomelo_context_root_options_s {
    /// @brief The allocator
    pomelo_allocator_t * allocator;

    /// @brief The capacity of a message
    size_t message_capacity;

    /// @brief Whether to synchronize the context
    bool synchronized;
};


struct pomelo_context_shared_options_s {
    /// @brief The allocator
    pomelo_allocator_t * allocator;

    /// @brief The original context. This could be root or shared context.
    /// The root context must be synchronized.
    pomelo_context_t * context;
};


struct pomelo_socket_options_s {
    /// @brief The API context
    pomelo_context_t * context;

    /// @brief The platform of client
    pomelo_platform_t * platform;

    /// @brief The number of channels
    size_t nchannels;

    /// @brief The array of channel modes.
    /// If this options is set NULL, all the channels will be set to unreliable
    /// mode.
    pomelo_channel_mode * channel_modes;
};


struct pomelo_session_iterator_s {
    /// @brief Signature of iterator
    uint64_t signature;

    /// @brief Current state of this iterator
    void * state;
};


struct pomelo_rtt_s {
    /// @brief Mean of round trip time
    uint64_t mean;

    /// @brief Variance of round trip time
    uint64_t variance;
};


/* -------------------------------------------------------------------------- */
/*                               Context APIs                                 */
/* -------------------------------------------------------------------------- */


/// @brief Create new root context
pomelo_context_t * pomelo_context_root_create(
    pomelo_context_root_options_t * options
);


/// @brief Create new shared context
pomelo_context_t * pomelo_context_shared_create(
    pomelo_context_shared_options_t * options
);


/// @brief Destroy context. Context param could be root or shared
void pomelo_context_destroy(pomelo_context_t * context);


/// @brief Set context extra data
void pomelo_context_set_extra(pomelo_context_t * context, void * data);


/// @brief Get context extra data
void * pomelo_context_get_extra(pomelo_context_t * context);


/// @brief Get the statistics
void pomelo_context_statistic(
    pomelo_context_t * context,
    pomelo_statistic_t * statistic
);


/// @brief Acquire a message from context
pomelo_message_t * pomelo_context_acquire_message(pomelo_context_t * context);


/* -------------------------------------------------------------------------- */
/*                                Socket APIs                                 */
/* -------------------------------------------------------------------------- */


/// @brief Create the socket
/// @param options The server creating options
/// @return New server or NULL on failure.
pomelo_socket_t * pomelo_socket_create(pomelo_socket_options_t * options);


/// @brief Destroy the socket.
/// @param socket The socket
void pomelo_socket_destroy(pomelo_socket_t * socket);


/// @brief Set socket extra data
void pomelo_socket_set_extra(pomelo_socket_t * socket, void * data);


/// @brief Get socket extra data
void * pomelo_socket_get_extra(pomelo_socket_t * socket);


/// @brief Start the socket as client and connect it to a server
/// @param socket The socket
/// @param connect_token The connect token
/// @return Returns 0 on success or -1 on failure
int pomelo_socket_connect(
    pomelo_socket_t * socket,
    const uint8_t * connect_token
);


/// @brief Start the socket as server and listen to connections
/// @param socket The socket
/// @param private_key The private key for server
/// @param protocol_id The special ID for particular app.
/// @param max_clients The maximum number of clients for this socket
/// @param address The bind address
int pomelo_socket_listen(
    pomelo_socket_t * socket,
    const uint8_t * private_key,
    uint64_t protocol_id,
    size_t max_clients,
    pomelo_address_t * address
);


/// @brief Stop listening or disconnect the socket
void pomelo_socket_stop(pomelo_socket_t * socket);


/// @brief Get the current state of socket
/// @param socket The socket
/// @return The current state of socket
pomelo_socket_state pomelo_socket_get_state(pomelo_socket_t * socket);


/// @brief Get number of connected sessions
size_t pomelo_socket_get_nsessions(pomelo_socket_t * socket);


/// @brief Send a message to multiple sessions.
/// After calling this function, the message WILL BE managed by socket.
/// @param socket The socket
/// @param channel_index The index of channel
/// @param message The message
/// @param sessions The sessions
/// @param nsessions The number of sessions
/// @param data The data for callback
void pomelo_socket_send(
    pomelo_socket_t * socket,
    size_t channel_index,
    pomelo_message_t * message,
    pomelo_session_t ** sessions,
    size_t nsessions,
    void * data
);


/// @brief Get the time of socket (Threadsafe)
uint64_t pomelo_socket_time(pomelo_socket_t * socket);


/// @brief Get context of socket
pomelo_context_t * pomelo_socket_get_context(pomelo_socket_t * socket);


/// @brief Get platform of socket
pomelo_platform_t * pomelo_socket_get_platform(pomelo_socket_t * socket);


/// @brief Get number of channels
size_t pomelo_socket_get_nchannels(pomelo_socket_t * socket);


/// @brief Get inital sending channel mode
pomelo_channel_mode pomelo_socket_get_channel_mode(
    pomelo_socket_t * socket,
    size_t channel_index
);


/// @brief Get the adapter
pomelo_adapter_t * pomelo_socket_get_adapter(pomelo_socket_t * socket);


/* -------------------------------------------------------------------------- */
/*                              Socket events                                 */
/* -------------------------------------------------------------------------- */

/// @brief Process when a session has established a connection
/// @param socket The socket
/// @param session The connected session
/// [External linkage]
void pomelo_socket_on_connected(
    pomelo_socket_t * socket,
    pomelo_session_t * session
);


/// @brief Process when a session has disconnected
/// @param socket The socket
/// @param session The disconnected session
/// [External linkage]
void pomelo_socket_on_disconnected(
    pomelo_socket_t * socket,
    pomelo_session_t * session
);


/// @brief Process when socket has received a message.
/// After this callback is called, the message will immediately be released.
/// @param socket The socket
/// @param session The session
/// @param message The received message
/// [External linkage]
void pomelo_socket_on_received(
    pomelo_socket_t * socket,
    pomelo_session_t * session,
    pomelo_message_t * message
);


/// @brief Process the connecting result
/// [External linkage]
void pomelo_socket_on_connect_result(
    pomelo_socket_t * socket,
    pomelo_socket_connect_result result
);


/// @brief Process the sending result. If auto-release is true, the message will
/// be released after this callback is called.
/// @param socket The socket
/// @param message The message
/// @param data The data for callback
/// @param send_count The number of sent messages
/// [External linkage]
void pomelo_socket_on_send_result(
    pomelo_socket_t * socket,
    pomelo_message_t * message,
    void * data,
    size_t send_count
);


/* -------------------------------------------------------------------------- */
/*                               Session APIs                                 */
/* -------------------------------------------------------------------------- */

/// @brief Set session extra data
void pomelo_session_set_extra(pomelo_session_t * session, void * data);


/// @brief Get session extra data
void * pomelo_session_get_extra(pomelo_session_t * session);


/// @brief Get the client ID of session
int64_t pomelo_session_get_client_id(pomelo_session_t * session);


/// @brief Get the address of session
pomelo_address_t * pomelo_session_get_address(pomelo_session_t * session);


/// @brief Send a message.
/// This is the shorter way to send message through a channel.
/// After calling this function, the message WILL BE managed by socket.
/// @param session The session
/// @param channel_index The index of channel
/// @param message The message
/// @param data The data for callback
void pomelo_session_send(
    pomelo_session_t * session,
    size_t channel_index,
    pomelo_message_t * message,
    void * data
);


/// @brief Disconnect a session.
/// All pending messages will be discarded. Trying to disconnect client session
/// will also stop the client.
int pomelo_session_disconnect(pomelo_session_t * session);


/// @brief Get the information of round trip time
/// @returns 0 on success, -1 on failure
int pomelo_session_get_rtt(pomelo_session_t * session, pomelo_rtt_t * rtt);


/// @brief Get the channel by index
/// @return The associated channel or NULL if the index is invalid
pomelo_channel_t * pomelo_session_get_channel(
    pomelo_session_t * session,
    size_t channel_index
);


/// @brief Shorter way to set channel mode of session
/// @return 0 on success or -1 on failure
int pomelo_session_set_channel_mode(
    pomelo_session_t * session,
    size_t channel_index,
    pomelo_channel_mode mode
);


/// @brief Shorter way to get the channel mode
/// @return Channel mode of session. If channel is not found or session is
/// invalid, UNRELIABLE will be
pomelo_channel_mode pomelo_session_get_channel_mode(
    pomelo_session_t * session,
    size_t channel_index
);


/// @brief Get the signature of session
uint64_t pomelo_session_get_signature(pomelo_session_t * session);


/// @brief Get socket of session
pomelo_socket_t * pomelo_session_get_socket(pomelo_session_t * session);


/// @brief Process when session is getting cleaned up
/// [External linkage]
void pomelo_session_on_cleanup(pomelo_session_t * session);


/* -------------------------------------------------------------------------- */
/*                               Channel APIs                                 */
/* -------------------------------------------------------------------------- */

/// @brief Set channel extra data
void pomelo_channel_set_extra(pomelo_channel_t * channel, void * data);


/// @brief Get channel extra data
void * pomelo_channel_get_extra(pomelo_channel_t * channel);


/// @brief Send a message through a channel.
/// @param channel The channel
/// @param message The message
/// @param data The data for callback
void pomelo_channel_send(
    pomelo_channel_t * channel,
    pomelo_message_t * message,
    void * data
);


/// @brief Change the delivery mode of a channel
/// @return 0 on success or -1 on failure
int pomelo_channel_set_mode(
    pomelo_channel_t * channel,
    pomelo_channel_mode mode
);


/// @brief Get the delivery mode of a channel
/// @return Channel mode of channel or UNRELIABLE if channel is not found.
pomelo_channel_mode pomelo_channel_get_mode(pomelo_channel_t * channel);


/// @brief Get session of channel
pomelo_session_t * pomelo_channel_get_session(pomelo_channel_t * channel);


/// @brief Process when channel is getting cleaned up
/// [External linkage]
void pomelo_channel_on_cleanup(pomelo_channel_t * channel);


/* -------------------------------------------------------------------------- */
/*                               Message APIs                                 */
/* -------------------------------------------------------------------------- */

/// @brief Set message extra data
void pomelo_message_set_extra(pomelo_message_t * message, void * data);


/// @brief Get message extra data
void * pomelo_message_get_extra(pomelo_message_t * message);


/// @brief Ref the message
/// @return 0 on success or -1 on failure
int pomelo_message_ref(pomelo_message_t * message);


/// @brief Unreference message.
/// @return 0 on success or -1 on failure
void pomelo_message_unref(pomelo_message_t * message);


/// @brief Change the message context.
void pomelo_message_set_context(
    pomelo_message_t * message,
    pomelo_context_t * context
);


/// @brief Get context of message
pomelo_context_t * pomelo_message_get_context(pomelo_message_t * message);


/// @brief Reset the message to make it empty and writable.
/// This will NOT modify the reference count of message.
/// @return 0 on success, or -1 on failure
int pomelo_message_reset(pomelo_message_t * message);


/// @brief Get the size of message
size_t pomelo_message_size(pomelo_message_t * message);


/// @brief Write buffer to the message
/// @return 0 on success, or -1 on failure
int pomelo_message_write_buffer(
    pomelo_message_t * message,
    const uint8_t * buffer,
    size_t length
);


int pomelo_message_write_uint8(pomelo_message_t * message, uint8_t value);
int pomelo_message_write_uint16(pomelo_message_t * message, uint16_t value);
int pomelo_message_write_uint32(pomelo_message_t * message, uint32_t value);
int pomelo_message_write_uint64(pomelo_message_t * message, uint64_t value);
int pomelo_message_write_float32(pomelo_message_t * message, float value);
int pomelo_message_write_float64(pomelo_message_t * message, double value);
int pomelo_message_write_int8(pomelo_message_t * message, int8_t value);
int pomelo_message_write_int16(pomelo_message_t * message, int16_t value);
int pomelo_message_write_int32(pomelo_message_t * message, int32_t value);
int pomelo_message_write_int64(pomelo_message_t * message, int64_t value);


/// @brief Reader buffer from message
/// @return 0 on success, or -1 on failure
int pomelo_message_read_buffer(
    pomelo_message_t * message,
    uint8_t * buffer,
    size_t length
);


int pomelo_message_read_uint8(pomelo_message_t * message, uint8_t * value);
int pomelo_message_read_uint16(pomelo_message_t * message, uint16_t * value);
int pomelo_message_read_uint32(pomelo_message_t * message, uint32_t * value);
int pomelo_message_read_uint64(pomelo_message_t * message, uint64_t * value);
int pomelo_message_read_float32(pomelo_message_t * message, float * value);
int pomelo_message_read_float64(pomelo_message_t * message, double * value);
int pomelo_message_read_int8(pomelo_message_t * message, int8_t * value);
int pomelo_message_read_int16(pomelo_message_t * message, int16_t * value);
int pomelo_message_read_int32(pomelo_message_t * message, int32_t * value);
int pomelo_message_read_int64(pomelo_message_t * message, int64_t * value);


/* -------------------------------------------------------------------------- */
/*                                Iterator APIs                               */
/* -------------------------------------------------------------------------- */


/// @brief Initialize the session iterator
int pomelo_session_iterator_init(
    pomelo_session_iterator_t * iterator,
    pomelo_socket_t * socket
);


/// @brief Iterate next element. If there are no more elements, p_element will
/// be stored NULL and 0 will be returned.
/// @param iterator Iterator
/// @param p_session Output session
/// @return 0 on success or -1 if original list has been modified.
int pomelo_session_iterator_next(
    pomelo_session_iterator_t * iterator,
    pomelo_session_t ** p_session
);


/// @brief Get a number of next elements in iterator to an array
/// @param iterator Iterator
/// @param max_elements Maximum number of elements which input array can store
/// @param array_sessions Output array of sessions
/// @return Number of loaded elements
size_t pomelo_session_iterator_load(
    pomelo_session_iterator_t * iterator,
    size_t max_elements,
    pomelo_session_t ** array_sessions
);


/* -------------------------------------------------------------------------- */
/*                                Plugin APIs                                 */
/* -------------------------------------------------------------------------- */

/// @brief Register a plugin. The result plugin will automatically released
/// when context is removed.
pomelo_plugin_t * pomelo_plugin_register(
    pomelo_allocator_t * allocator,
    pomelo_context_t * context,
    pomelo_platform_t * platform,
    pomelo_plugin_initializer initializer
);


/// @brief Load initialier from shared library by its name
pomelo_plugin_initializer pomelo_plugin_load_by_name(const char * name);


/// @brief Load initialier from shared library by its path
pomelo_plugin_initializer pomelo_plugin_load_by_path(const char * path);


#ifdef __cplusplus
}
#endif
#endif // POMELO_API_H
