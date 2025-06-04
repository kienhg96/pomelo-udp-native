#ifndef POMELO_DELIVERY_ENDPOINT_SRC_H
#define POMELO_DELIVERY_ENDPOINT_SRC_H
#include "utils/list.h"
#include "utils/array.h"
#include "base/extra.h"
#include "delivery.h"
#include "fragment.h"
#include "clock.h"
#include "heartbeat.h"
#ifdef __cplusplus
extern "C" {
#endif


// Reliable capacity configuration (encoded in 3 bits)
#define POMELO_MIN_RELIABLE_CAPACITY 8
#define POMELO_MAX_RELIABLE_CAPACITY 1024
#define POMELO_DEFAULT_RELIABLE_CAPACITY 8


/**
 * System parcel meta byte layout:
 *    opcode: 3 bits
 *    opcode-meta: 5 bits - specified by the opcode
 * 
 * Ping opcode-meta layout:
 *    sequence_bytes - 1: 1 bit
 *    time_sync: 1 bit
 *    reliable_cap_exp: 3 bits
 * 
 * Ping content layout:
 *    sequence: 1 - 8 bytes
 * 
 * Pong opcode-meta layout:
 *    sequence_bytes - 1: 1 bit
 *    time_sync: 1 bit
 *    time_bytes - 1: 3 bits
 * 
 * Pong content layout (Only available when time_sync is true):
 *    sequence: 1 - 8 bytes
 *    time    : 1 - 8 bytes
 */


/// @brief The time sync flag of the endpoint
#define POMELO_DELIVERY_ENDPOINT_FLAG_TIME_SYNC  (1 << 0)

/// @brief The ready flag of the endpoint
#define POMELO_DELIVERY_ENDPOINT_FLAG_READY      (1 << 1)

/// @brief The ping frequency of the endpoint
#define POMELO_DELIVERY_ENDPOINT_PING_FREQUENCY 10 // Hz

/// @brief Send result
typedef struct pomelo_delivery_send_result_s pomelo_delivery_send_result_t;


/// @brief The opcode of the parcel
typedef enum pomelo_delivery_opcode {
    POMELO_DELIVERY_OPCODE_PING = 0,
    POMELO_DELIVERY_OPCODE_PONG = 1
} pomelo_delivery_opcode;


struct pomelo_delivery_send_result_s {
    /// @brief The context
    pomelo_delivery_context_t * context;

    /// @brief The endpoint
    pomelo_delivery_endpoint_t * endpoint;

    /// @brief The parcel
    pomelo_delivery_parcel_t * parcel;

    /// @brief The send count
    size_t send_count;
};


struct pomelo_delivery_endpoint_s {
    /// @brief The extra data of endpoint
    pomelo_extra_t extra;

    /// @brief The flags of this endpoint
    uint32_t flags;

    /// @brief The context of this endpoint
    pomelo_delivery_context_t * context;

    /// @brief The platform of this endpoint
    pomelo_platform_t * platform;

    /// @brief Heartbeat of this endpoint
    pomelo_delivery_heartbeat_t * heartbeat;

    /// @brief The buses array of this endpoint
    pomelo_array_t * buses;

    /// @brief The system bus of this endpoint
    pomelo_delivery_bus_t * system_bus;

    /// @brief The sequencer of this endpoint
    pomelo_sequencer_t * sequencer;

    /// @brief The RTT calculator of this endpoint
    pomelo_rtt_calculator_t rtt;

    /// @brief The clock of this endpoint
    pomelo_delivery_clock_t clock;

    /// @brief The heartbeat ping handle
    pomelo_delivery_heartbeat_handle_t heartbeat_handle;

    /// @brief The stop task
    pomelo_sequencer_task_t stop_task;

    /// @brief The destroy task
    pomelo_sequencer_task_t destroy_task;

    /// @brief The heartbeat task
    pomelo_sequencer_task_t heartbeat_task;

    /// @brief The ready task
    pomelo_sequencer_task_t ready_task;

    /// @brief Capacity of outgoing reliable message heap.
    /// This is the maximum number of reliable messages can be sent parallelly.
    /// By current specs, the maximum of this value is 128
    size_t out_reliable_capacity;

    /// @brief Capacity of incoming reliable message heap.
    /// This is the maximum number of reliable messages can be received
    /// parallelly.
    /// By current specs, the maximum of this value is 128
    size_t in_reliable_capacity;
};


/// @brief Allocate the endpoint callback for pool
int pomelo_delivery_endpoint_on_alloc(
    pomelo_delivery_endpoint_t * endpoint,
    pomelo_delivery_context_t * context
);


/// @brief Free the endpoint callback for pool
void pomelo_delivery_endpoint_on_free(pomelo_delivery_endpoint_t * endpoint);


/// @brief Initialize the endpoint
int pomelo_delivery_endpoint_init(
    pomelo_delivery_endpoint_t * endpoint,
    pomelo_delivery_endpoint_options_t * info
);


/// @brief Cleanup the endpoint
void pomelo_delivery_endpoint_cleanup(pomelo_delivery_endpoint_t * endpoint);


/// @brief Send the ping of the endpoint
void pomelo_delivery_endpoint_send_ping(pomelo_delivery_endpoint_t * endpoint);


/// @brief Send the ping of the endpoint with the parcel
void pomelo_delivery_endpoint_send_ping_ex(
    pomelo_delivery_endpoint_t * endpoint,
    pomelo_delivery_parcel_t * parcel
);


/// @brief Send the pong of the endpoint
void pomelo_delivery_endpoint_send_pong(
    pomelo_delivery_endpoint_t * endpoint,
    uint64_t sequence,
    bool time_sync
);


/// @brief Send the pong of the endpoint with the parcel
void pomelo_delivery_endpoint_send_pong_ex(
    pomelo_delivery_endpoint_t * endpoint,
    uint64_t sequence,
    bool time_sync,
    pomelo_delivery_parcel_t * parcel
);


/// @brief Receive the system parcel
void pomelo_delivery_endpoint_recv_system_parcel(
    pomelo_delivery_endpoint_t * endpoint,
    pomelo_delivery_parcel_t * parcel
);


/// @brief Receive the ping of the endpoint.
void pomelo_delivery_endpoint_recv_ping(
    pomelo_delivery_endpoint_t * endpoint,
    uint8_t meta_byte,
    pomelo_delivery_reader_t * reader
);


/// @brief Receive the pong of the endpoint.
void pomelo_delivery_endpoint_recv_pong(
    pomelo_delivery_endpoint_t * endpoint,
    uint8_t meta_byte,
    pomelo_delivery_reader_t * reader
);


/// @brief Process heartbeat
void pomelo_delivery_endpoint_heartbeat(pomelo_delivery_endpoint_t * endpoint);


/// @brief The deferred stop function
void pomelo_delivery_endpoint_stop_deferred(
    pomelo_delivery_endpoint_t * endpoint
);


/// @brief The deferred destroy function
void pomelo_delivery_endpoint_destroy_deferred(
    pomelo_delivery_endpoint_t * endpoint
);


#ifdef __cplusplus
}
#endif
#endif // POMELO_DELIVERY_ENDPOINT_SRC_H
