#ifndef POMELO_PROTOCOL_SERVER_SRC_H
#define POMELO_PROTOCOL_SERVER_SRC_H
#include <stdbool.h>
#include "protocol.h"
#include "platform/platform.h"
#include "utils/pool.h"
#include "utils/map.h"
#include "utils/macro.h"
#include "socket.h"
#include "packet.h"


#ifdef __cplusplus
extern "C" {
#endif


struct pomelo_protocol_server_s {
    /// @brief The base socket
    pomelo_protocol_socket_t socket;

    /// @brief The address map for connected peers.
    /// Map from address to peer.
    pomelo_map_t * peer_address_map;

    /// @brief The requesting peers
    pomelo_list_t * requesting_peers;

    /// @brief The challenging peers
    pomelo_list_t * challenging_peers;

    /// @brief The denied peers
    pomelo_list_t * denied_peers;

    /// @brief The connected peers
    pomelo_list_t * connected_peers;

    /// @brief The disconnecting peers
    pomelo_list_t * disconnecting_peers;

    /// @brief The private key for decoding request packets
    uint8_t private_key[POMELO_KEY_BYTES];
    
    /// @brief The challenge key
    uint8_t challenge_key[POMELO_KEY_BYTES];

    /// @brief The maximum number of clients
    size_t max_clients;

    /// @brief The protocol ID of server
    uint64_t protocol_id;

    /// @brief The bind address of server
    pomelo_address_t address;

    /// @brief The sequence number for anonymous peer
    uint64_t anonymous_sequence_number;

    /// @brief The challenge packet sequence number
    uint64_t challenge_sequence_number;

    /// @brief The keep alive timer
    pomelo_platform_handle_t keep_alive_timer;

    /// @brief The timer for sending redundants disconnect packets
    pomelo_platform_handle_t disconnecting_timer;

    /// @brief The timer for removing expired anonymous peers
    pomelo_platform_handle_t anonymous_timer;

    /// @brief The task for keep alive
    pomelo_sequencer_task_t keep_alive_task;

    /// @brief The task for sending redundants disconnect packets
    pomelo_sequencer_task_t disconnecting_task;

    /// @brief The task for removing expired anonymous peers
    pomelo_sequencer_task_t scan_challenging_task;
};


/// @brief The callback on server allocated
int pomelo_protocol_server_on_alloc(
    pomelo_protocol_server_t * server,
    pomelo_protocol_context_t * context
);


/// @brief The callback on server freed
void pomelo_protocol_server_on_free(pomelo_protocol_server_t * server);


/// @brief Initialize the server
int pomelo_protocol_server_init(
    pomelo_protocol_server_t * server,
    pomelo_protocol_server_options_t * options
);


/// @brief Cleanup the server
void pomelo_protocol_server_cleanup(pomelo_protocol_server_t * server);


/// @brief Destroy the server
void pomelo_protocol_server_destroy(pomelo_protocol_server_t * server);


/// @brief The socket server starts listening
/// @return 0 on success, or an error code < 0 on failure
int pomelo_protocol_server_start(pomelo_protocol_server_t * server);


/// @brief Stop the server
void pomelo_protocol_server_stop(pomelo_protocol_server_t * server);


/// @brief Validate the incoming packet
int pomelo_protocol_server_validate(
    pomelo_protocol_server_t * server,
    pomelo_protocol_packet_incoming_t * incoming,
    pomelo_protocol_packet_validation_t * validation
);


/// @brief Process the packet before sending
void pomelo_protocol_server_presend_packet(
    pomelo_protocol_server_t * server,
    pomelo_protocol_peer_t * peer
);


/* -------------------------------------------------------------------------- */
/*                          Server incoming packets                           */
/* -------------------------------------------------------------------------- */

/// @brief Process the incoming packet
void pomelo_protocol_server_recv_packet(
    pomelo_protocol_server_t * server,
    pomelo_protocol_peer_t * peer,
    pomelo_protocol_packet_t * packet
);


/// @brief Process the incoming packet failed
void pomelo_protocol_server_recv_failed(
    pomelo_protocol_server_t * server,
    pomelo_protocol_peer_t * peer,
    pomelo_protocol_packet_header_t * header
);


/// @brief Process request packet
void pomelo_protocol_server_recv_request(
    pomelo_protocol_server_t * server,
    pomelo_protocol_peer_t * peer,
    pomelo_protocol_packet_request_t * packet
);


/// @brief Process request packet failed
void pomelo_protocol_server_recv_request_failed(
    pomelo_protocol_server_t * server,
    pomelo_protocol_peer_t * peer,
    pomelo_protocol_packet_header_t * header
);


/// @brief Process the response packet
void pomelo_protocol_server_recv_response(
    pomelo_protocol_server_t * server,
    pomelo_protocol_peer_t * peer,
    pomelo_protocol_packet_response_t * packet
);


/// @brief Process disconnect packet
void pomelo_protocol_server_recv_disconnect(
    pomelo_protocol_server_t * server,
    pomelo_protocol_peer_t * peer,
    pomelo_protocol_packet_disconnect_t * packet
);


/// @brief Process keep alive packet
void pomelo_protocol_server_recv_keep_alive(
    pomelo_protocol_server_t * server,
    pomelo_protocol_peer_t * peer,
    pomelo_protocol_packet_keep_alive_t * packet
);


/* -------------------------------------------------------------------------- */
/*                             Outgoing packets                               */
/* -------------------------------------------------------------------------- */

/// @brief Process sent packet
void pomelo_protocol_server_sent_packet(
    pomelo_protocol_server_t * server,
    pomelo_protocol_peer_t * peer,
    pomelo_protocol_packet_t * packet
);


/// @brief Process after sending the denied packet
void pomelo_protocol_server_sent_denied(
    pomelo_protocol_server_t * server,
    pomelo_protocol_peer_t * peer
);


/* -------------------------------------------------------------------------- */
/*                        Server specific functions                           */
/* -------------------------------------------------------------------------- */

/// @brief Broadcast keep alive to all connected peers
void pomelo_protocol_server_broadcast_keep_alive(
    pomelo_protocol_server_t * server
);


/// @brief Send the connection challenge packet
int pomelo_protocol_server_send_challenge(
    pomelo_protocol_server_t * server,
    pomelo_protocol_peer_t * peer,
    pomelo_protocol_packet_request_t * request_packet
);


/// @brief Send the connection denied packet.
/// The anonymous peer will automatically removed after sending is done.
int pomelo_protocol_server_send_denied(
    pomelo_protocol_server_t * server,
    pomelo_protocol_peer_t * peer
);


/// @brief Send keep alive packet to target peer
int pomelo_protocol_server_send_keep_alive(
    pomelo_protocol_server_t * server,
    pomelo_protocol_peer_t * peer
);


/// @brief Acquire a peer from the server and add it to address map
pomelo_protocol_peer_t * pomelo_protocol_server_acquire_peer(
    pomelo_protocol_server_t * server,
    pomelo_address_t * address
);


/// @brief Release a peer from the server and remove it from address map
void pomelo_protocol_server_release_peer(
    pomelo_protocol_server_t * server,
    pomelo_protocol_peer_t * peer
);


/// @brief Disconnect a connected peer
int pomelo_protocol_server_disconnect_peer(
    pomelo_protocol_server_t * server,
    pomelo_protocol_peer_t * peer
);


/// @brief Send redundant disconnect packets to all disconnecting peers
void pomelo_protocol_server_broadcast_disconnect(
    pomelo_protocol_server_t * server
);


/// @brief Send a redundant disconnect packet to a peer
int pomelo_protocol_server_send_disconnect_peer(
    pomelo_protocol_server_t * server,
    pomelo_protocol_peer_t * peer
);


/// @brief Scan challenging peers and remove expired ones
void pomelo_protocol_server_scan_challenging_peers(
    pomelo_protocol_server_t * server
);


/// @brief Deny a peer
void pomelo_protocol_server_deny_peer(
    pomelo_protocol_server_t * server,
    pomelo_protocol_peer_t * peer
);


#ifdef __cplusplus
}
#endif
#endif // POMELO_PROTOCOL_SERVER_SRC_H
