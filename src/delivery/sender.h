#ifndef POMELO_DELIVERY_SEND_BATCH_SRC_H
#define POMELO_DELIVERY_SEND_BATCH_SRC_H
#include "base/pipeline.h"
#include "base/extra.h"
#include "utils/list.h"
#include "delivery.h"
#include "internal.h"
#ifdef __cplusplus
extern "C" {
#endif


#define POMELO_DELIVERY_SENDER_FLAG_CANCELED (1 << 0)
#define POMELO_DELIVERY_SENDER_FLAG_FAILED   (1 << 1)
#define POMELO_DELIVERY_SENDER_FLAG_SYSTEM   (1 << 2)


/// @brief A single transmission of a sender
typedef struct pomelo_delivery_transmission_s pomelo_delivery_transmission_t;


struct pomelo_delivery_transmission_s {
    /// @brief The bus of this recipient
    pomelo_delivery_bus_t * bus;

    /// @brief The mode of this recipient
    pomelo_delivery_mode mode;
};


struct pomelo_delivery_sender_s {
    /// @brief The pipeline of this command
    pomelo_pipeline_t pipeline;

    /// @brief Extra data of sender
    pomelo_extra_t extra;

    /// @brief The context of this command
    pomelo_delivery_context_t * context;

    /// @brief The platform of this command
    pomelo_platform_t * platform;

    /// @brief The recipients of this command
    pomelo_list_t * records;

    /// @brief The dispatchers of this command
    pomelo_list_t * dispatchers;

    /// @brief The sending parcel
    pomelo_delivery_parcel_t * parcel;

    /// @brief The number of completed sending processes
    size_t completed_counter;

    /// @brief The number of successful sending processes
    size_t success_counter;

    /// @brief The flags of this command
    uint32_t flags;

    /// @brief The task for updating the checksum
    pomelo_platform_handle_t checksum_update_task;

    /// @brief The checksum of the parcel
    int checksum_compute_result;

    /// @brief The buffer for checksum
    pomelo_buffer_t * checksum;
};


/* -------------------------------------------------------------------------- */
/*                           Sending Batch Command                            */
/* -------------------------------------------------------------------------- */

/// @brief The callback of sending batch command on allocation
int pomelo_delivery_sender_on_alloc(
    pomelo_delivery_sender_t * sender,
    pomelo_delivery_context_t * context
);


/// @brief The callback of sending batch command on free
void pomelo_delivery_sender_on_free(pomelo_delivery_sender_t * sender);


/// @brief Initialize the sending batch command
int pomelo_delivery_sender_init(
    pomelo_delivery_sender_t * sender,
    pomelo_delivery_sender_options_t * info
);


/// @brief Cleanup the sending batch command
void pomelo_delivery_sender_cleanup(pomelo_delivery_sender_t * sender);


/// @brief Update the checksum of the sending batch command
void pomelo_delivery_sender_update_checksum(pomelo_delivery_sender_t * sender);


/// @brief Dispatch the sending batch command
void pomelo_delivery_sender_dispatch(pomelo_delivery_sender_t * sender);


/// @brief Complete the sending batch command
void pomelo_delivery_sender_complete(pomelo_delivery_sender_t * sender);


/// @brief Handle the complete event of dispatcher
void pomelo_delivery_sender_on_dispatcher_result(
    pomelo_delivery_sender_t * sender,
    pomelo_delivery_dispatcher_t * command
);


#ifdef __cplusplus
}
#endif
#endif // POMELO_DELIVERY_SEND_BATCH_SRC_H
