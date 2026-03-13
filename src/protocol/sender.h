#ifndef POMELO_PROTOCOL_SENDER_SRC_H
#define POMELO_PROTOCOL_SENDER_SRC_H
#include "protocol.h"
#include "peer.h"
#include "protocol/packet.h"
#include "platform/platform.h"
#include "base/pipeline.h"
#include "utils/list.h"

#ifdef __cplusplus
extern "C" {
#endif


#define POMELO_PROTOCOL_SENDER_FLAG_CANCELED   (1 << 0) // Canceled
#define POMELO_PROTOCOL_SENDER_FLAG_NO_ENCRYPT (1 << 1) // No encryption
#define POMELO_PROTOCOL_SENDER_FLAG_FAILED     (1 << 2) // Failed


/// @brief The sender information
typedef struct pomelo_protocol_sender_info_s pomelo_protocol_sender_info_t;


struct pomelo_protocol_sender_info_s {
    /// @brief The peer
    pomelo_protocol_peer_t * peer;

    /// @brief The packet
    pomelo_protocol_packet_t * packet;

    /// @brief The flags
    uint32_t flags;
};


struct pomelo_protocol_sender_s {
    /// @brief The pipeline
    pomelo_pipeline_t pipeline;

    /// @brief The platform
    pomelo_platform_t * platform;

    /// @brief The context
    pomelo_protocol_context_t * context;

    /// @brief The socket
    pomelo_protocol_socket_t * socket;

    /// @brief The target peer
    pomelo_protocol_peer_t * peer;

    /// @brief Flags
    uint32_t flags;
    
    /// @brief The packet to send
    pomelo_protocol_packet_t * packet;

    /// @brief The codec context
    pomelo_protocol_crypto_context_t * codec_ctx;

    /// @brief The async task
    pomelo_platform_handle_t task;

    /// @brief Entry of this sender in peer senders list
    pomelo_list_entry_t * entry;

    /// @brief The processed buffer view
    pomelo_buffer_view_t view;

    /// @brief The result of processing
    int process_result;

};


/// @brief Initialize the sender
int pomelo_protocol_sender_init(
    pomelo_protocol_sender_t * sender,
    pomelo_protocol_sender_info_t * info
);


/// @brief Cleanup the sender
void pomelo_protocol_sender_cleanup(pomelo_protocol_sender_t * sender);


/// @brief Submit a sender
void pomelo_protocol_sender_submit(pomelo_protocol_sender_t * sender);


/// @brief Process the sender
void pomelo_protocol_sender_process(pomelo_protocol_sender_t * sender);


/// @brief Dispatch the sender
void pomelo_protocol_sender_dispatch(pomelo_protocol_sender_t * sender);


/// @brief Complete the sender
void pomelo_protocol_sender_complete(pomelo_protocol_sender_t * sender);


/// @brief Cancel the sender
void pomelo_protocol_sender_cancel(pomelo_protocol_sender_t * sender);


#ifdef __cplusplus
}
#endif
#endif // POMELO_PROTOCOL_SENDER_SRC_H
