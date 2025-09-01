#ifndef POMELO_PROTOCOL_RECEIVER_SRC_H
#define POMELO_PROTOCOL_RECEIVER_SRC_H
#include "protocol.h"
#include "peer.h"
#include "protocol/packet.h"
#include "platform/platform.h"
#include "base/pipeline.h"
#include "utils/list.h"

#ifdef __cplusplus
extern "C" {
#endif


#define POMELO_PROTOCOL_RECEIVER_FLAG_CANCELED   (1 << 0) // Canceled
#define POMELO_PROTOCOL_RECEIVER_FLAG_NO_DECRYPT (1 << 1) // No decryption
#define POMELO_PROTOCOL_RECEIVER_FLAG_FAILED     (1 << 2) // Failed


/// @brief The receiver information
typedef struct pomelo_protocol_receiver_info_s pomelo_protocol_receiver_info_t;


struct pomelo_protocol_receiver_info_s {
    /// @brief The sender peer
    pomelo_protocol_peer_t * peer;

    /// @brief The body view
    pomelo_buffer_view_t * body_view;

    /// @brief The header
    pomelo_protocol_packet_header_t * header;

    /// @brief The flags
    uint32_t flags;
};


struct pomelo_protocol_receiver_s {
    /// @brief The pipeline of receiver
    pomelo_pipeline_t pipeline;

    /// @brief The platform of receiver
    pomelo_platform_t * platform;

    /// @brief The context of receiver
    pomelo_protocol_context_t * context;

    /// @brief The socket of receiver
    pomelo_protocol_socket_t * socket;

    /// @brief Sender peer
    pomelo_protocol_peer_t * peer;

    /// @brief Flags of receiver
    uint32_t flags;

    /// @brief The processing packet
    pomelo_protocol_packet_t * packet;

    /// @brief The codec context
    pomelo_protocol_crypto_context_t * crypto_ctx;

    /// @brief The async task of this receiver
    pomelo_platform_handle_t task;

    /// @brief Entry of this receiver in peer receivers list
    pomelo_list_entry_t * entry;

    /// @brief The view of received packet body
    pomelo_buffer_view_t body_view;

    /// @brief The header of received packet
    pomelo_protocol_packet_header_t header;

    /// @brief Received time
    uint64_t recv_time;

    /// @brief The result of processing
    int process_result;
};


/// @brief Initialize the receiver
int pomelo_protocol_receiver_init(
    pomelo_protocol_receiver_t * receiver,
    pomelo_protocol_receiver_info_t * info
);


/// @brief Release the receiver
void pomelo_protocol_receiver_cleanup(pomelo_protocol_receiver_t * receiver);


/// @brief Submit the receiver
void pomelo_protocol_receiver_submit(pomelo_protocol_receiver_t * receiver);


/// @brief Process the receiver
void pomelo_protocol_receiver_process(pomelo_protocol_receiver_t * receiver);


/// @brief Complete the receiver
void pomelo_protocol_receiver_complete(pomelo_protocol_receiver_t * receiver);


/// @brief Cancel the receiver
void pomelo_protocol_receiver_cancel(pomelo_protocol_receiver_t * receiver);


#ifdef __cplusplus
}
#endif
#endif // POMELO_PROTOCOL_RECEIVER_SRC_H
