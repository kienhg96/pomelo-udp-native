#ifndef POMELO_BASE_ALLOCATOR_SRC_H
#define POMELO_BASE_ALLOCATOR_SRC_H
#include "pomelo/allocator.h"
#include "pomelo/statistic/statistic-allocator.h"
#include "utils/atomic.h"
#ifdef __cplusplus
extern "C" {
#endif


struct pomelo_allocator_s {
    /// @brief The allocator context
    void * context;

    /// @brief The allocator malloc
    pomelo_alloc_callback malloc;

    /// @brief The allocator free
    pomelo_free_callback free;

    /// @brief The allocator realloc
    pomelo_realloc_callback realloc;

    /// @brief Failure callback
    pomelo_alloc_failure_callback failure_callback;

    /// @brief Total allocated bytes
    pomelo_atomic_uint64_t allocated_bytes;

    /// @brief Whether this allocator is default allocator
    bool is_default;

#ifndef NDEBUG
    /// @brief The signature of allocator
    int signature;

    /// @brief The signature of all memory blocks created by this allocator
    int element_signature;
#endif

};

struct pomelo_allocator_header_s;

/// @brief The header for allocator
typedef struct pomelo_allocator_header_s pomelo_allocator_header_t;


struct pomelo_allocator_header_s {
    /// @brief The size of memory
    size_t size;

#ifndef NDEBUG
    /// @brief The signature of memory block
    int signature;
#endif
};


/// @brief Get the statistic of allocator
void pomelo_allocator_statistic(
    pomelo_allocator_t * allocator,
    pomelo_statistic_allocator_t * statistic
);


/// @brief Get the size of memory block
size_t pomelo_allocator_sizeof(void * ptr);


#ifdef __cplusplus
}
#endif // __cplusplus
#endif // POMELO_BASE_ALLOCATOR_SRC_H
