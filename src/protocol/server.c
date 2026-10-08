// Specific functions for socket servers
#include <assert.h>
#include <string.h>
#include "pomelo/random.h"
#include "utils/macro.h"
#include "socket.h"
#include "peer.h"
#include "server.h"
#include "context.h"


/* -------------------------------------------------------------------------- */
/*                              Server internal                               */
/* -------------------------------------------------------------------------- */

static int64_t pomelo_map_address_hash(
    pomelo_map_t * map,
    void * callback_context,
    pomelo_address_t * address
) {
    (void) map;
    (void) callback_context;
    return pomelo_address_hash(address);
}

static bool pomelo_map_address_compare(
    pomelo_map_t * map,
    void * callback_context,
    pomelo_address_t * first,
    pomelo_address_t * second
) {
    (void) map;
    (void) callback_context;
    return pomelo_address_compare(first, second);
}


/* -------------------------------------------------------------------------- */
/*                               Public APIs                                  */
/* -------------------------------------------------------------------------- */


int pomelo_protocol_server_on_alloc(
    pomelo_protocol_server_t * server,
    pomelo_protocol_context_t * context
) {
    assert(server != NULL);
    assert(context != NULL);

    int ret = pomelo_protocol_socket_on_alloc(&server->socket, context);
    if (ret < 0) return ret; // Failed to initialize socket


    pomelo_allocator_t * allocator = context->allocator;

    // Create address to peer maps
    pomelo_map_options_t map_options = {
        .allocator = allocator,
        .key_size = sizeof(pomelo_address_t),
        .value_size = sizeof(pomelo_protocol_peer_t *),
        .hash_fn = (pomelo_map_hash_fn) pomelo_map_address_hash,
        .compare_fn = (pomelo_map_compare_fn) pomelo_map_address_compare
    };
    server->peer_address_map = pomelo_map_create(&map_options);
    if (!server->peer_address_map) return -1; // Failed to create new map

    pomelo_list_options_t list_options = {
        .allocator = allocator,
        .element_size = sizeof(pomelo_protocol_peer_t *)
    };

    // Create requesting peers list
    server->requesting_peers = pomelo_list_create(&list_options);
    if (!server->requesting_peers) return -1; // Failed to create new list

    // Create anonymous peers list
    server->challenging_peers = pomelo_list_create(&list_options);
    if (!server->challenging_peers) return -1; // Failed to create new list

    // Create denied peers list
    server->denied_peers = pomelo_list_create(&list_options);
    if (!server->denied_peers) return -1; // Failed to create new list

    // Create connected peers list
    server->connected_peers = pomelo_list_create(&list_options);
    if (!server->connected_peers) return -1; // Failed to create new list

    // Create disconnecting peers list
    server->disconnecting_peers = pomelo_list_create(&list_options);
    if (!server->disconnecting_peers) return -1; // Failed to create new list

    return 0;
}


void pomelo_protocol_server_on_free(pomelo_protocol_server_t * server) {
    assert(server != NULL);

    if (server->peer_address_map) {
        pomelo_map_destroy(server->peer_address_map);
        server->peer_address_map = NULL;
    }

    if (server->requesting_peers) {
        pomelo_list_destroy(server->requesting_peers);
        server->requesting_peers = NULL;
    }

    if (server->challenging_peers) {
        pomelo_list_destroy(server->challenging_peers);
        server->challenging_peers = NULL;
    }

    if (server->denied_peers) {
        pomelo_list_destroy(server->denied_peers);
        server->denied_peers = NULL;
    }

    if (server->connected_peers) {
        pomelo_list_destroy(server->connected_peers);
        server->connected_peers = NULL;
    }

    if (server->disconnecting_peers) {
        pomelo_list_destroy(server->disconnecting_peers);
        server->disconnecting_peers = NULL;
    }

    pomelo_protocol_socket_on_free(&server->socket);
}



/// @brief Initialize the server
int pomelo_protocol_server_init(
    pomelo_protocol_server_t * server,
    pomelo_protocol_server_options_t * options
) {
    assert(server != NULL);
    assert(options != NULL);
    if (!options->platform || !options->adapter) return -1;

    // Check capability of adapter
    uint32_t flags = 0;
    uint32_t capability = pomelo_adapter_get_capability(options->adapter);

    // Check if the adapter supports server
    if (!(capability & POMELO_ADAPTER_CAPABILITY_SERVER_ALL)) {
        return -1; // No supported capabilities
    }

    // Check if the adapter supports encryption
    if (!(capability & POMELO_ADAPTER_CAPABILITY_SERVER_ENCRYPTED)) {
        flags |= POMELO_PROTOCOL_SOCKET_FLAG_NO_ENCRYPT;
    }

    pomelo_protocol_socket_t * socket = &server->socket;
    pomelo_protocol_socket_options_t socket_options = {
        .platform = options->platform,
        .adapter = options->adapter,
        .sequencer = options->sequencer,
        .mode = POMELO_PROTOCOL_SOCKET_MODE_SERVER,
        .flags = flags
    };
    int ret = pomelo_protocol_socket_init(socket, &socket_options);
    if (ret < 0) return ret;


    memcpy(server->private_key, options->private_key, POMELO_KEY_BYTES);
    memset(server->challenge_key, 0, POMELO_KEY_BYTES);
    server->max_clients = options->max_clients;
    server->protocol_id = options->protocol_id;
    server->address = options->address;
    server->anonymous_sequence_number = 0;
    server->challenge_sequence_number = 0;

    // Initialize the tasks
    pomelo_sequencer_task_init(
        &server->keep_alive_task,
        (pomelo_sequencer_callback) pomelo_protocol_server_broadcast_keep_alive,
        server
    );

    // Initialize the broadcast disconnect task
    pomelo_sequencer_task_init(
        &server->disconnecting_task,
        (pomelo_sequencer_callback) pomelo_protocol_server_broadcast_disconnect,
        server
    );

    // Initialize the scan challenging peers task
    pomelo_sequencer_task_init(
        &server->scan_challenging_task,
        (pomelo_sequencer_callback)
            pomelo_protocol_server_scan_challenging_peers,
        server
    );

    return 0;
}


void pomelo_protocol_server_cleanup(pomelo_protocol_server_t * server) {
    assert(server != NULL);
    pomelo_protocol_socket_cleanup(&server->socket);
}


pomelo_protocol_socket_t * pomelo_protocol_server_create(
    pomelo_protocol_server_options_t * options
) {
    assert(options != NULL);
    if (!options->context) return NULL; // No context is provided

    return (pomelo_protocol_socket_t *)
        pomelo_pool_acquire(options->context->server_pool, options);
}


void pomelo_protocol_server_destroy(pomelo_protocol_server_t * server) {
    assert(server != NULL);
    pomelo_pool_release(server->socket.context->server_pool, server);
}


int pomelo_protocol_server_validate(
    pomelo_protocol_server_t * server,
    pomelo_protocol_packet_incoming_t * incoming,
    pomelo_protocol_packet_validation_t * validation
) {
    assert(server != NULL);
    assert(incoming != NULL);
    assert(validation != NULL);

    pomelo_address_t * address = incoming->address;
    pomelo_buffer_view_t * view = incoming->view;

    pomelo_protocol_packet_header_t * header = &validation->header;

    // Server ignores challenge & denied packets
    pomelo_protocol_packet_type type = header->type;
    if (type == POMELO_PROTOCOL_PACKET_CHALLENGE ||
        type == POMELO_PROTOCOL_PACKET_DENIED) {
        // Invalid packet type
        return -1;
    }

    // Get the peer
    pomelo_protocol_peer_t * peer = NULL;
    pomelo_protocol_peer_state state = POMELO_PROTOCOL_PEER_DISCONNECTED;

    // Check the peer out and protect server from packet replay
    pomelo_map_get(server->peer_address_map, *address, &peer);
    if (peer) {
        state = peer->state;
    }

    switch (type) {
        // Only server packet
        case POMELO_PROTOCOL_PACKET_REQUEST:
            // Only available for new peers
            if (peer) return -1;
            break;

        case POMELO_PROTOCOL_PACKET_RESPONSE:
            // Peer must be created before receiving response packet
            if (!peer || state != POMELO_PROTOCOL_PEER_CHALLENGE) {
                return -1;
            }

            // Check if the peer is processing response packet
            if (peer->flags & POMELO_PEER_FLAG_PROCESSING_RESPONSE) {
                return -1; // Already processing response packet
            }
            break;

        // Replay protection
        case POMELO_PROTOCOL_PACKET_PAYLOAD:
        case POMELO_PROTOCOL_PACKET_KEEP_ALIVE:
        case POMELO_PROTOCOL_PACKET_DISCONNECT:
            // Not available for unconnected peers
            if (!peer || state != POMELO_PROTOCOL_PEER_CONNECTED) return -1;

            // Replay protection
            if (
                pomelo_protocol_peer_protect_replay(peer, header->sequence) < 0
            ) return -1;
            break;

        default:
            return -1; // Other cases are discarded
    }

    if (type != POMELO_PROTOCOL_PACKET_REQUEST) {
        validation->peer = peer;
        return 0;
    }

    // Customized cases for request packet
    pomelo_payload_t payload;
    payload.data = view->buffer->data + view->offset;
    payload.position = POMELO_VERSION_INFO_BYTES; // Skip version info
    payload.capacity = view->length;
    uint64_t protocol_id = 0;

    // Quick check protocol ID
    int ret = pomelo_payload_read_uint64(&payload, &protocol_id);
    if (ret < 0 || protocol_id != server->protocol_id) {
        return -1; // Failed to read protocol ID or mismatch
    }

    // Create new anonymous peer
    if (!peer) {
        // Bound the number of half-open (pending) handshakes. An unauthenticated
        // REQUEST (protocol_id is public) otherwise allocates a peer and a
        // decryption worker task before the token is validated, so a spoofed
        // flood could exhaust memory/CPU. Only requesting_peers needs this cap:
        // challenging_peers is only reachable with a valid (decrypted) token.
        // Dropping is safe because clients retransmit their REQUEST.
        if (server->requesting_peers->size >= server->max_clients) {
            return -1; // Too many pending handshakes; drop this REQUEST
        }

        peer = pomelo_protocol_server_acquire_peer(server, address);
        if (!peer) {
            return -1; // Cannot allocate new anonymous peer
        }

        peer->entry = pomelo_list_push_back(server->requesting_peers, peer);
        if (!peer->entry) {
            // Error when trying to move peer to anonymous list, release peer
            pomelo_protocol_server_release_peer(server, peer);
            return -1;
        }

        peer->state = POMELO_PROTOCOL_PEER_REQUEST;
    }

    // Update the codec context for anonymous peer
    pomelo_protocol_crypto_context_t * codec_ctx = peer->crypto_ctx;

    // Protocol ID, encrypt and decrypt keys will be set later
    codec_ctx->protocol_id = 0;
    memset(codec_ctx->packet_decrypt_key, 0, POMELO_KEY_BYTES);
    memset(codec_ctx->packet_encrypt_key, 0, POMELO_KEY_BYTES);
    memcpy(
        codec_ctx->challenge_key,
        server->challenge_key,
        POMELO_KEY_BYTES
    );
    memcpy(
        codec_ctx->private_key,
        server->private_key,
        POMELO_KEY_BYTES
    );

    validation->peer = peer;
    return 0;
}


void pomelo_protocol_server_presend_packet(
    pomelo_protocol_server_t * server,
    pomelo_protocol_peer_t * peer
) {
    assert(peer != NULL);

    // With unconfirmed sessions, we need to send a keep alive packet
    // along with payload packet
    if (!(peer->flags & POMELO_PEER_FLAG_CONFIRMED)) {
        pomelo_protocol_server_send_keep_alive(server, peer);
    }
}


/* -------------------------------------------------------------------------- */
/*                              Server packets                                */
/* -------------------------------------------------------------------------- */

void pomelo_protocol_server_recv_packet(
    pomelo_protocol_server_t * server,
    pomelo_protocol_peer_t * peer,
    pomelo_protocol_packet_t * packet
) {
    assert(server != NULL);
    assert(peer != NULL);
    assert(packet != NULL);

    switch (packet->type) {
        case POMELO_PROTOCOL_PACKET_REQUEST:
            pomelo_protocol_server_recv_request(
                server, peer, (pomelo_protocol_packet_request_t *) packet
            );
            break;

        case POMELO_PROTOCOL_PACKET_RESPONSE:
            pomelo_protocol_server_recv_response(
                server, peer, (pomelo_protocol_packet_response_t *) packet
            );
            break;

        case POMELO_PROTOCOL_PACKET_DISCONNECT:
            pomelo_protocol_server_recv_disconnect(
                server, peer, (pomelo_protocol_packet_disconnect_t *) packet
            );
            break;

        case POMELO_PROTOCOL_PACKET_KEEP_ALIVE:
            pomelo_protocol_server_recv_keep_alive(
                server, peer, (pomelo_protocol_packet_keep_alive_t *) packet
            );
            break;

        default:
            break;
    }
}


void pomelo_protocol_server_recv_failed(
    pomelo_protocol_server_t * server,
    pomelo_protocol_peer_t * peer,
    pomelo_protocol_packet_header_t * header
) {
    assert(header != NULL);
    switch (header->type) {
        case POMELO_PROTOCOL_PACKET_REQUEST:
            pomelo_protocol_server_recv_request_failed(server, peer, header);
            break;

        default:
            break;
    }
}


void pomelo_protocol_server_recv_request(
    pomelo_protocol_server_t * server,
    pomelo_protocol_peer_t * peer,
    pomelo_protocol_packet_request_t * packet
) {
    assert(server != NULL);
    assert(peer != NULL);
    assert(packet != NULL);

    if (peer->state != POMELO_PROTOCOL_PEER_REQUEST) {
        return; // Ignore
    }

    if (server->connected_peers->size >= server->max_clients) {
        // Full of slots, move the peer to denied list and send denied packet
        pomelo_protocol_server_deny_peer(server, peer);
        return;
    }

    // We get the token as raw private connect token
    pomelo_connect_token_t * token = &packet->token_data.token;
    peer->client_id = token->client_id; // Set the client ID
    peer->last_recv_time = pomelo_platform_hrtime(server->socket.platform);
    peer->timeout_ns = POMELO_SECONDS_TO_NS(token->timeout);

    // Copy user data
    memcpy(
        peer->user_data,
        token->user_data,
        POMELO_USER_DATA_BYTES
    );

    // Update codec protocol id, encrypt & decrypt keys
    peer->crypto_ctx->protocol_id = token->protocol_id;
    memcpy(
        peer->crypto_ctx->packet_decrypt_key,
        token->client_to_server_key,
        POMELO_KEY_BYTES
    );
    memcpy(
        peer->crypto_ctx->packet_encrypt_key,
        token->server_to_client_key,
        POMELO_KEY_BYTES
    );

    // Move peer to challenging list
    pomelo_list_remove(server->requesting_peers, peer->entry);
    peer->entry = pomelo_list_push_back(server->challenging_peers, peer);
    if (!peer->entry) {
        // Error when trying to move peer to challenging list, release peer
        pomelo_protocol_server_release_peer(server, peer);
        return;
    }
    peer->state = POMELO_PROTOCOL_PEER_CHALLENGE;

    // Response with challenge packet
    pomelo_protocol_server_send_challenge(server, peer, packet);
}


void pomelo_protocol_server_recv_request_failed(
    pomelo_protocol_server_t * server,
    pomelo_protocol_peer_t * peer,
    pomelo_protocol_packet_header_t * header
) {
    assert(server != NULL);
    assert(peer != NULL);
    (void) header;

    if (peer->state != POMELO_PROTOCOL_PEER_REQUEST) {
        return; // Ignore
    }

    // Deny the peer
    pomelo_protocol_server_deny_peer(server, peer);
}


void pomelo_protocol_server_recv_response(
    pomelo_protocol_server_t * server,
    pomelo_protocol_peer_t * peer,
    pomelo_protocol_packet_response_t * packet
) {
    assert(server != NULL);
    assert(peer != NULL);
    assert(packet != NULL);

    if (peer->state != POMELO_PROTOCOL_PEER_CHALLENGE) {
        return; // Ignore
    }

    pomelo_challenge_token_t * challenge_token = &packet->challenge_data.token;
    if (peer->client_id != challenge_token->client_id) {
        return; // Mismatch client ID
    }

    if (memcmp(
        peer->user_data,
        challenge_token->user_data,
        POMELO_USER_DATA_BYTES
    ) != 0) {
        return; // Mismatch user data
    }

    // Remove the peer from anonymous list
    pomelo_list_remove(server->challenging_peers, peer->entry);

    // Add to connected peers, and put the peer to connected map
    peer->entry = pomelo_list_push_back(server->connected_peers, peer);
    if (!peer->entry) {
        // Error when trying to move peer to connected list, release peer
        pomelo_protocol_server_release_peer(server, peer);
        return;
    }
    peer->state = POMELO_PROTOCOL_PEER_CONNECTED;
    peer->flags &= ~POMELO_PEER_FLAG_CONFIRMED;

    // Send keep alive packet
    pomelo_protocol_server_send_keep_alive(server, peer);

    // Finally, call the callback
    pomelo_protocol_socket_on_connected(&server->socket, peer);
}


void pomelo_protocol_server_recv_disconnect(
    pomelo_protocol_server_t * server,
    pomelo_protocol_peer_t * peer,
    pomelo_protocol_packet_disconnect_t * packet
) {
    assert(server != NULL);
    assert(peer != NULL);
    (void) packet;

    if (peer->state != POMELO_PROTOCOL_PEER_CONNECTED) {
        return; // Ignore
    }

    // Update peer state and call the callback
    peer->state = POMELO_PROTOCOL_PEER_DISCONNECTED;
    pomelo_protocol_socket_dispatch_peer_disconnected(&server->socket, peer);

    // Remove the peer from connected list
    pomelo_list_remove(server->connected_peers, peer->entry);
    peer->entry = NULL;

    // Finally, release the peer
    pomelo_protocol_server_release_peer(server, peer);
}


void pomelo_protocol_server_recv_keep_alive(
    pomelo_protocol_server_t * server,
    pomelo_protocol_peer_t * peer,
    pomelo_protocol_packet_keep_alive_t * packet
) {
    assert(server != NULL);
    assert(peer != NULL);
    assert(packet != NULL);

    if (peer->state != POMELO_PROTOCOL_PEER_CONNECTED) {
        return; // Ignore
    }

    if (peer->flags & POMELO_PEER_FLAG_CONFIRMED) {
        return; // Already confirmed
    }

    if (peer->client_id != packet->client_id) {
        return; // Mismatch client ID
    }

    peer->flags |= POMELO_PEER_FLAG_CONFIRMED;
}


/* -------------------------------------------------------------------------- */
/*                             Outgoing packets                               */
/* -------------------------------------------------------------------------- */

void pomelo_protocol_server_sent_packet(
    pomelo_protocol_server_t * server,
    pomelo_protocol_peer_t * peer,
    pomelo_protocol_packet_t * packet
) {
    assert(packet != NULL);

    switch (packet->type) {
        case POMELO_PROTOCOL_PACKET_DENIED:
            pomelo_protocol_server_sent_denied(server, peer);
            break;

        default:
            break;
    }
}


void pomelo_protocol_server_sent_denied(
    pomelo_protocol_server_t * server,
    pomelo_protocol_peer_t * peer
) {
    assert(server != NULL);
    assert(peer != NULL);

    if (peer->state != POMELO_PROTOCOL_PEER_DENIED) {
        return; // Ignore
    }

    // Remove the peer from denied list
    pomelo_list_remove(server->denied_peers, peer->entry);

    // Then release the peer
    pomelo_protocol_server_release_peer(server, peer);
}


/* -------------------------------------------------------------------------- */
/*                         Server specific functions                          */
/* -------------------------------------------------------------------------- */


static void process_keep_alive(pomelo_protocol_server_t * server) {
    assert(server != NULL);
    pomelo_sequencer_submit(
        server->socket.sequencer,
        &server->keep_alive_task
    );
}


static void process_broadcast_disconnect(pomelo_protocol_server_t * server) {
    assert(server != NULL);
    pomelo_sequencer_submit(
        server->socket.sequencer,
        &server->disconnecting_task
    );
}


static void process_scan_challenging_peers(pomelo_protocol_server_t * server) {
    assert(server != NULL);
    pomelo_sequencer_submit(
        server->socket.sequencer,
        &server->scan_challenging_task
    );
}


int pomelo_protocol_server_start(pomelo_protocol_server_t * server) {
    assert(server != NULL);
    pomelo_protocol_socket_t * socket = (pomelo_protocol_socket_t *) server;

    // Initialize randomly challenge key
    pomelo_random_buffer(server->challenge_key, POMELO_KEY_BYTES);

    // Start adapter as server
    int ret = pomelo_adapter_listen(socket->adapter, &server->address);
    if (ret < 0) return ret; // Failed to listen

    // Start timers
    pomelo_platform_t * platform = socket->platform;

    // Start the keep alive interval
    ret = pomelo_platform_timer_start(
        platform,
        (pomelo_platform_timer_entry) process_keep_alive,
        0, // No timeout
        POMELO_FREQ_TO_MS(POMELO_KEEP_ALIVE_FREQUENCY_HZ),
        server,
        &server->keep_alive_timer
    );
    if (ret < 0) {
        // Cannot start new timer, stop the socket.
        pomelo_protocol_server_stop(server);
        return ret;
    }

    // Start looping disconnect scanning
    ret = pomelo_platform_timer_start(
        platform,
        (pomelo_platform_timer_entry) process_broadcast_disconnect,
        0, // No timeout
        POMELO_FREQ_TO_MS(POMELO_DISCONNECT_FREQUENCY_HZ),
        server,
        &server->disconnecting_timer
    );
    if (ret < 0) {
        // Cannot start new timer, stop the socket.
        pomelo_protocol_server_stop(server);
        return ret;
    }

    ret = pomelo_platform_timer_start(
        platform,
        (pomelo_platform_timer_entry) process_scan_challenging_peers,
        0, // No timeout
        POMELO_FREQ_TO_MS(POMELO_ANONYMOUS_REMOVAL_FREQUENCY_HZ),
        server,
        &server->anonymous_timer
    );
    if (ret < 0) {
        // Cannot start new timer, stop the socket.
        pomelo_protocol_server_stop(server);
        return ret;
    }

    return 0;
}


void pomelo_protocol_server_broadcast_keep_alive(
    pomelo_protocol_server_t * server
) {
    assert(server != NULL);

    pomelo_protocol_socket_t * socket = (pomelo_protocol_socket_t *) server;
    pomelo_list_t * peers = server->connected_peers;
    if (peers->size == 0) {
        return; // No connected clients, nothing to do
    }

    // Get the current time
    uint64_t time_ns = pomelo_platform_hrtime(socket->platform);

    pomelo_protocol_peer_t * peer;
    pomelo_list_iterator_t it;
    pomelo_list_iterator_init(&it, peers);

    while (pomelo_list_iterator_next(&it, &peer) == 0) {
        uint64_t elapsed_ns = time_ns - peer->last_recv_time;
        if (elapsed_ns <= peer->timeout_ns) {
            // Send the keep alive to peer
            pomelo_protocol_server_send_keep_alive(server, peer);
            continue;
        }

        // Timed out, call the disconnect callback
        peer->state = POMELO_PROTOCOL_PEER_DISCONNECTED;
        pomelo_protocol_socket_dispatch_peer_disconnected(socket, peer);

        // Then remove the peer from connected list
        pomelo_list_iterator_remove(&it);

        // Then release the peer
        pomelo_protocol_server_release_peer(server, peer);
    }
}


int pomelo_protocol_server_send_challenge(
    pomelo_protocol_server_t * server,
    pomelo_protocol_peer_t * peer,
    pomelo_protocol_packet_request_t * request_packet
) {
    assert(server != NULL);
    assert(peer != NULL);
    assert(request_packet != NULL);
    assert(peer->state == POMELO_PROTOCOL_PEER_CHALLENGE);

    pomelo_protocol_socket_t * socket = (pomelo_protocol_socket_t *) server;
    pomelo_connect_token_t * token = &request_packet->token_data.token;

    pomelo_protocol_packet_challenge_info_t info = {
        .sequence = server->anonymous_sequence_number++,
        .token_sequence = server->challenge_sequence_number++,
        .client_id = token->client_id,
        .user_data = token->user_data,
    };
    pomelo_protocol_packet_challenge_t * packet = pomelo_pool_acquire(
        socket->context->packet_pools[POMELO_PROTOCOL_PACKET_CHALLENGE],
        &info
    );
    if (!packet) return -1; // Failed to acquire packet

    pomelo_protocol_socket_dispatch(socket, peer, &packet->base);
    return 0;
}


int pomelo_protocol_server_send_denied(
    pomelo_protocol_server_t * server,
    pomelo_protocol_peer_t * peer
) {
    assert(server != NULL);
    assert(peer != NULL);
    assert(peer->state == POMELO_PROTOCOL_PEER_DENIED);

    pomelo_protocol_socket_t * socket = (pomelo_protocol_socket_t *) server;
    pomelo_protocol_packet_denied_info_t info = {
        .sequence = server->anonymous_sequence_number++
    };
    pomelo_protocol_packet_denied_t * packet = pomelo_pool_acquire(
        socket->context->packet_pools[POMELO_PROTOCOL_PACKET_DENIED],
        &info
    );
    if (!packet) return -1; // Failed to acquire packet

    pomelo_protocol_socket_dispatch(socket, peer, &packet->base);

    // => pomelo_protocol_server_sent_denied
    return 0;
}


int pomelo_protocol_server_send_keep_alive(
    pomelo_protocol_server_t * server,
    pomelo_protocol_peer_t * peer
) {
    assert(server != NULL);
    assert(peer != NULL);
    assert(peer->state == POMELO_PROTOCOL_PEER_CONNECTED);

    pomelo_protocol_socket_t * socket = (pomelo_protocol_socket_t *) server;
    pomelo_protocol_packet_keep_alive_info_t info = {
        .sequence = pomelo_protocol_peer_next_sequence(peer),
        .client_id = peer->client_id
    };

    pomelo_protocol_packet_keep_alive_t * packet = pomelo_pool_acquire(
        socket->context->packet_pools[POMELO_PROTOCOL_PACKET_KEEP_ALIVE],
        &info
    );
    if (!packet) return -1; // Failed to acquire packet

    // Finally, dispatch the packet
    pomelo_protocol_socket_dispatch(socket, peer, &packet->base);
    return 0;
}


pomelo_protocol_peer_t * pomelo_protocol_server_acquire_peer(
    pomelo_protocol_server_t * server,
    pomelo_address_t * address
) {
    assert(server != NULL);
    assert(address != NULL);

    pomelo_protocol_socket_t * socket = (pomelo_protocol_socket_t *) server;
    pomelo_protocol_context_t * context = socket->context;

    // Acquire new peer
    pomelo_protocol_peer_info_t info = {
        .socket = socket,
        .created_time_ns = pomelo_platform_hrtime(socket->platform),
    };
    pomelo_protocol_peer_t * peer =
        pomelo_pool_acquire(context->peer_pool, &info);
    if (!peer) return NULL; // Cannot allocate new peer

    // Set the address
    peer->address = *address;

    // Set to address map
    pomelo_map_entry_t * entry = pomelo_map_set(
        server->peer_address_map,
        *address,
        peer
    );
    if (!entry) {
        pomelo_pool_release(context->peer_pool, peer);
        return NULL; // Failed to set to map
    }

    return peer;
}


void pomelo_protocol_server_release_peer(
    pomelo_protocol_server_t * server,
    pomelo_protocol_peer_t * peer
) {
    assert(server != NULL);
    assert(peer != NULL);

    // Cancel all senders and receivers
    pomelo_protocol_peer_cancel_senders_and_receivers(peer);

    // Remove from address map
    pomelo_map_del(server->peer_address_map, peer->address);

    // Release the peer
    pomelo_pool_release(server->socket.context->peer_pool, peer);
}


void pomelo_protocol_server_stop(pomelo_protocol_server_t * server) {
    assert(server != NULL);
    pomelo_protocol_socket_t * socket = (pomelo_protocol_socket_t *) server;

    // Stop the adapter
    pomelo_adapter_stop(socket->adapter);

    pomelo_platform_t * platform = server->socket.platform;
    pomelo_platform_timer_stop(platform, &server->keep_alive_timer);
    pomelo_platform_timer_stop(platform, &server->disconnecting_timer);
    pomelo_platform_timer_stop(platform, &server->anonymous_timer);

    // Reset all values
    server->anonymous_sequence_number = 0;
    server->challenge_sequence_number = 0;
    memset(server->private_key, 0, POMELO_KEY_BYTES);
    memset(server->challenge_key, 0, POMELO_KEY_BYTES);

    // Remove all connected peers
    pomelo_protocol_peer_t * peer;
    pomelo_pool_t * peer_pool = server->socket.context->peer_pool;

    pomelo_list_t * requesting_peers = server->requesting_peers;
    while (pomelo_list_pop_front(requesting_peers, &peer) == 0) {
        pomelo_protocol_peer_cancel_senders_and_receivers(peer);
        pomelo_pool_release(peer_pool, peer);
    }

    pomelo_list_t * challenging_peers = server->challenging_peers;
    while (pomelo_list_pop_front(challenging_peers, &peer) == 0) {
        pomelo_protocol_peer_cancel_senders_and_receivers(peer);
        pomelo_pool_release(peer_pool, peer);
    }

    pomelo_list_t * denied_peers = server->denied_peers;
    while (pomelo_list_pop_front(denied_peers, &peer) == 0) {
        pomelo_protocol_peer_cancel_senders_and_receivers(peer);
        pomelo_pool_release(peer_pool, peer);
    }

    pomelo_list_t * connected_peers = server->connected_peers;
    while (pomelo_list_pop_front(connected_peers, &peer) == 0) {
        pomelo_protocol_peer_cancel_senders_and_receivers(peer);
        pomelo_pool_release(peer_pool, peer);
    }

    pomelo_list_t * disconnecting_peers = server->disconnecting_peers;
    while (pomelo_list_pop_front(disconnecting_peers, &peer) == 0) {
        pomelo_protocol_peer_cancel_senders_and_receivers(peer);
        pomelo_pool_release(peer_pool, peer);
    }

    // Remove all mapping
    pomelo_map_clear(server->peer_address_map);
}


int pomelo_protocol_server_disconnect_peer(
    pomelo_protocol_server_t * server,
    pomelo_protocol_peer_t * peer
) {
    assert(server != NULL);
    assert(peer != NULL);

    if (peer->state != POMELO_PROTOCOL_PEER_CONNECTED) {
        return -1; // Cannot disconnect a peer that is not connected
    }

    // Move the peer to disconnecting list
    pomelo_list_remove(server->connected_peers, peer->entry);
    peer->entry = pomelo_list_push_back(server->disconnecting_peers, peer);
    if (!peer->entry) {
        peer->state = POMELO_PROTOCOL_PEER_DISCONNECTED;

        // Failed to move to disconnecting list, dispatch callback first
        pomelo_protocol_socket_dispatch_peer_disconnected(&server->socket, peer);

        // Then remove the orphan peer
        pomelo_protocol_server_release_peer(server, peer);
        return -1;
    }

    peer->state = POMELO_PROTOCOL_PEER_DISCONNECTING;
    peer->remain_redundant_disconnect = POMELO_DISCONNECT_REDUNDANT_LIMIT;

    // Call the callback
    pomelo_protocol_socket_dispatch_peer_disconnected(&server->socket, peer);
    return 0;
}


void pomelo_protocol_server_broadcast_disconnect(
    pomelo_protocol_server_t * server
) {
    assert(server != NULL);

    pomelo_list_t * disconnecting_peers = server->disconnecting_peers;
    if (disconnecting_peers->size == 0) {
        return; // No peer to send disconnect packet
    }

    pomelo_protocol_peer_t * peer;
    pomelo_list_iterator_t it;

    pomelo_list_iterator_init(&it, disconnecting_peers);
    while (pomelo_list_iterator_next(&it, &peer) == 0) {
        int remain = peer->remain_redundant_disconnect--;
        if (remain > 0) {
            pomelo_protocol_server_send_disconnect_peer(server, peer);
            continue;
        }

        // Remove the peer from disconnecting list
        pomelo_list_iterator_remove(&it);

        // Then release the peer
        pomelo_protocol_server_release_peer(server, peer);
    }
}


int pomelo_protocol_server_send_disconnect_peer(
    pomelo_protocol_server_t * server,
    pomelo_protocol_peer_t * peer
) {
    assert(server != NULL);
    assert(peer != NULL);
    assert(peer->state == POMELO_PROTOCOL_PEER_DISCONNECTING);

    pomelo_protocol_socket_t * socket = (pomelo_protocol_socket_t *) server;
    pomelo_protocol_packet_disconnect_info_t info = {
        .sequence = pomelo_protocol_peer_next_sequence(peer)
    };
    pomelo_protocol_packet_disconnect_t * packet = pomelo_pool_acquire(
        socket->context->packet_pools[POMELO_PROTOCOL_PACKET_DISCONNECT],
        &info
    );
    if (!packet) return -1; // Failed to acquire packet

    // Dispatch the packet
    pomelo_protocol_socket_dispatch(socket, peer, &packet->base);
    return 0;
}


void pomelo_protocol_server_scan_challenging_peers(
    pomelo_protocol_server_t * server
) {
    assert(server != NULL);

    pomelo_protocol_socket_t * socket = (pomelo_protocol_socket_t *) server;
    pomelo_list_t * challenging_peers = server->challenging_peers;
    if (challenging_peers->size == 0) {
        return; // No anonymous peers, nothing to do
    }

    pomelo_protocol_peer_t * peer;
    pomelo_list_iterator_t it;
    uint64_t current_time_ns = pomelo_platform_hrtime(socket->platform);

    pomelo_list_iterator_init(&it, challenging_peers);
    while (pomelo_list_iterator_next(&it, &peer) == 0) {
        uint64_t elapsed_ns = current_time_ns - peer->created_time_ns;
        if (elapsed_ns > peer->timeout_ns) {
            // Remove the peer from challenging list
            pomelo_list_iterator_remove(&it);

            // Then release the peer
            pomelo_protocol_server_release_peer(server, peer);
            // No denied packet will be sent for this peer
        }
    }
}


void pomelo_protocol_server_deny_peer(
    pomelo_protocol_server_t * server,
    pomelo_protocol_peer_t * peer
) {
    assert(server != NULL);
    assert(peer != NULL);
    assert(
        peer->state == POMELO_PROTOCOL_PEER_REQUEST ||
        peer->state == POMELO_PROTOCOL_PEER_CHALLENGE
    );

    // Remove peer from the requesting or challenging list
    switch (peer->state) {
        case POMELO_PROTOCOL_PEER_REQUEST:
            pomelo_list_remove(server->requesting_peers, peer->entry);
            break;

        case POMELO_PROTOCOL_PEER_CHALLENGE:
            pomelo_list_remove(server->challenging_peers, peer->entry);
            break;

        default:
            return; // Invalid state
    }

    // Add peer to denied list
    peer->entry = pomelo_list_push_back(server->denied_peers, peer);
    if (!peer->entry) {
        // Error when trying to move peer to denied list, release peer
        pomelo_protocol_server_release_peer(server, peer);
        return;
    }
    peer->state = POMELO_PROTOCOL_PEER_DENIED;

    // Send denied packet
    int ret = pomelo_protocol_server_send_denied(server, peer);
    if (ret < 0) {
        // Failed to send denied packet, remove peer from denied list
        pomelo_list_remove(server->denied_peers, peer->entry);

        // Then release the peer
        pomelo_protocol_server_release_peer(server, peer);
    }
}
