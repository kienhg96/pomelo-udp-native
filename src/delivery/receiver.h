#ifndef POMELO_DELIVERY_RECV_SRC_H
#define POMELO_DELIVERY_RECV_SRC_H
#include "base/pipeline.h"
#include "crypto/checksum.h"
#include "utils/heap.h"
#include "utils/map.h"
#include "utils/array.h"
#include "internal.h"
#include "delivery.h"
#include "fragment.h"

#ifdef __cplusplus
extern "C" {
#endif


#define POMELO_DELIVERY_RECEIVER_FLAG_CANCELED           (1 << 0)
#define POMELO_DELIVERY_RECEIVER_FLAG_FAILED             (1 << 1)
#define POMELO_DELIVERY_RECEIVER_FLAG_COMPUTING_CHECKSUM (1 << 2)


/// @brief The information of the receiver
typedef struct pomelo_delivery_receiver_info_s pomelo_delivery_receiver_info_t;


struct pomelo_delivery_receiver_s {
    /// @brief The pipeline
    pomelo_pipeline_t pipeline;

    /// @brief The context
    pomelo_delivery_context_t * context;

    /// @brief The platform
    pomelo_platform_t * platform;

    /// @brief The receiving bus
    pomelo_delivery_bus_t * bus;

    /// @brief The delivery mode of this parcel
    pomelo_delivery_mode mode;
    
    /// @brief The sequence of the parcel
    uint64_t sequence;

    /// @brief The number of received fragments
    size_t recv_fragments;

    /// @brief The array of received fragments
    pomelo_array_t * fragments;

    /// @brief The expired time of this command (unreliable & sequenced only)
    uint64_t expired_time;

    /// @brief The entry of this command in expired heap
    pomelo_heap_entry_t * expired_entry;

    /// @brief The entry of this command in sequence map
    pomelo_map_entry_t * sequence_entry;

    /// @brief The flags of this command
    uint32_t flags;

    /// @brief The task of checksum verification
    pomelo_platform_handle_t checksum_verify_task;

    /// @brief The result of checksum computation
    int checksum_compute_result;

    /// @brief The embedded checksum of the parcel
    uint8_t * embedded_checksum;

    /// @brief The computed checksum of the parcel
    uint8_t computed_checksum[POMELO_CRYPTO_CHECKSUM_BYTES];
};


struct pomelo_delivery_receiver_info_s {
    /// @brief The bus of this receiver
    pomelo_delivery_bus_t * bus;

    /// @brief The fragment meta of this command
    pomelo_delivery_fragment_meta_t * meta;
};


/* -------------------------------------------------------------------------- */
/*                             Receiving Command                              */
/* -------------------------------------------------------------------------- */

/// @brief The on alloc function of the receiving command
int pomelo_delivery_receiver_on_alloc(
    pomelo_delivery_receiver_t * receiver,
    pomelo_delivery_context_t * context
);


/// @brief The on free function of the receiving command
void pomelo_delivery_receiver_on_free(pomelo_delivery_receiver_t * receiver);


/// @brief Initialize the receiving command
int pomelo_delivery_receiver_init(
    pomelo_delivery_receiver_t * receiver,
    pomelo_delivery_receiver_info_t * info
);


/// @brief Cleanup the receiving command
void pomelo_delivery_receiver_cleanup(pomelo_delivery_receiver_t * receiver);


/// @brief Start executing the receiving command
void pomelo_delivery_receiver_submit(pomelo_delivery_receiver_t * receiver);


/// @brief Check the meta of the receiving command
int pomelo_delivery_receiver_check_meta(
    pomelo_delivery_receiver_t * receiver,
    pomelo_delivery_fragment_meta_t * meta
);


/// @brief Cancel the receiving command
void pomelo_delivery_receiver_cancel(pomelo_delivery_receiver_t * receiver);


/// @brief Wait for the receiving command to receive all fragments
void pomelo_delivery_receiver_wait_fragments(
    pomelo_delivery_receiver_t * receiver
);


/// @brief Add a fragment to the receiving command
void pomelo_delivery_receiver_add_fragment(
    pomelo_delivery_receiver_t * receiver,
    pomelo_delivery_fragment_meta_t * meta,
    pomelo_buffer_view_t * content
);


/// @brief Verify the checksum of the receiving command
void pomelo_delivery_receiver_verify_checksum(
    pomelo_delivery_receiver_t * receiver
);


/// @brief Complete the receiving command
void pomelo_delivery_receiver_complete(pomelo_delivery_receiver_t * receiver);


/// @brief Compare two receiving commands by their expiration time
int pomelo_delivery_receiver_compare_expiration(
    pomelo_delivery_receiver_t * receiver_a,
    pomelo_delivery_receiver_t * receiver_b
);


#ifdef __cplusplus
}
#endif
#endif // POMELO_DELIVERY_RECV_SRC_H
