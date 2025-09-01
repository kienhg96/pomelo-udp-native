#ifndef POMELO_DELIVERY_DISPATCH_SRC_H
#define POMELO_DELIVERY_DISPATCH_SRC_H
#include "platform/platform.h"
#include "utils/list.h"
#include "utils/array.h"
#include "base/pipeline.h"
#include "internal.h"
#include "delivery.h"
#include "fragment.h"
#ifdef __cplusplus
extern "C" {
#endif


#define POMELO_DELIVERY_DISPATCHER_FLAG_CANCELED (1 << 0)
#define POMELO_DELIVERY_DISPATCHER_FLAG_FAILED   (1 << 1)


/// @brief The information of the dispatcher
typedef struct pomelo_delivery_dispatcher_info_s
    pomelo_delivery_dispatcher_info_t;


/// @brief The callback of dispatcher
typedef void (*pomelo_delivery_dispatcher_cb)(
    pomelo_delivery_dispatcher_t * dispatcher
);


typedef enum pomelo_delivery_checksum_mode {
    /// @brief No checksum
    POMELO_DELIVERY_CHECKSUM_NONE,

    /// @brief Checksum is embedded in the last fragment
    POMELO_DELIVERY_CHECKSUM_EMBEDDED,
    
    /// @brief An extra fragment is used to carry the checksum
    POMELO_DELIVERY_CHECKSUM_EXTRA

} pomelo_delivery_checksum_mode;


struct pomelo_delivery_dispatcher_s {
    /// @brief The pipeline of this command
    pomelo_pipeline_t pipeline;

    /// @brief The context of this command
    pomelo_delivery_context_t * context;

    /// @brief The platform of this command
    pomelo_platform_t * platform;

    /// @brief The sending bus
    pomelo_delivery_bus_t * bus;

    /// @brief The sending endpoint
    pomelo_delivery_endpoint_t * endpoint;

    /// @brief The fragments of this parcel
    pomelo_array_t * fragments;

    /// @brief The number of acknowledged fragments
    size_t acked_counter;

    /// @brief The delivery mode of this parcel
    pomelo_delivery_mode mode;

    /// @brief The sequence of this command
    uint64_t sequence;

    /// @brief The sequencer of this command
    pomelo_sequencer_t * sequencer;

    /// @brief The timer for resending command (reliable only)
    pomelo_platform_handle_t resend_timer;

    /// @brief The resend task
    pomelo_sequencer_task_t resend_task;

    /// @brief The flags of this command
    uint32_t flags;

    /// @brief The mode of checksum attachment
    pomelo_delivery_checksum_mode checksum_mode;

    /// @brief The buffer for checksum
    pomelo_buffer_t * checksum;

    /// @brief The sender of this command
    pomelo_delivery_sender_t * sender;

    /// @brief The entry of this command in the sender's sub-commands list
    pomelo_list_entry_t * sender_entry;
};


struct pomelo_delivery_dispatcher_info_s {
    /// @brief The bus of this command
    pomelo_delivery_bus_t * bus;

    /// @brief The parcel of this command
    pomelo_delivery_parcel_t * parcel;

    /// @brief The sender of this command
    pomelo_delivery_sender_t * sender;

    /// @brief The sending mode of this command
    pomelo_delivery_mode mode;

    /// @brief The sequence of this command
    uint64_t sequence;
};


/* -------------------------------------------------------------------------- */
/*                             Sending Command                                */
/* -------------------------------------------------------------------------- */

/// @brief The callback of dispatcher on alloc
int pomelo_delivery_dispatcher_on_alloc(
    pomelo_delivery_dispatcher_t * dispatcher,
    pomelo_delivery_context_t * context
);


/// @brief The callback of dispatcher on free
void pomelo_delivery_dispatcher_on_free(
    pomelo_delivery_dispatcher_t * dispatcher
);


/// @brief Initialize the dispatcher
int pomelo_delivery_dispatcher_init(
    pomelo_delivery_dispatcher_t * dispatcher,
    pomelo_delivery_dispatcher_info_t * info
);


/// @brief Cleanup the dispatcher
void pomelo_delivery_dispatcher_cleanup(
    pomelo_delivery_dispatcher_t * dispatcher
);


/// @brief Cancel the dispatcher
void pomelo_delivery_dispatcher_cancel(
    pomelo_delivery_dispatcher_t * dispatcher
);


/// @brief Start executing the dispatcher
void pomelo_delivery_dispatcher_submit(
    pomelo_delivery_dispatcher_t * dispatcher
);


/// @brief Start dispatching the parcel
void pomelo_delivery_dispatcher_dispatch(
    pomelo_delivery_dispatcher_t * dispatcher
);


/// @brief Send the parcel once
int pomelo_delivery_dispatcher_send(pomelo_delivery_dispatcher_t * dispatcher);


/// @brief Resend the parcel
void pomelo_delivery_dispatcher_resend(
    pomelo_delivery_dispatcher_t * dispatcher
);


/// @brief Submit ack response to sending command
void pomelo_delivery_dispatcher_recv_ack(
    pomelo_delivery_dispatcher_t * dispatcher,
    pomelo_delivery_fragment_meta_t * meta
);


/// @brief Complete the dispatcher
void pomelo_delivery_dispatcher_complete(
    pomelo_delivery_dispatcher_t * dispatcher
);


#ifdef __cplusplus
}
#endif
#endif // POMELO_DELIVERY_DISPATCH_SRC_H
