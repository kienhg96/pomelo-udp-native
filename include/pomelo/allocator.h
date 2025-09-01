#ifndef POMELO_ALLOCATOR_H
#define POMELO_ALLOCATOR_H
#include <stddef.h>
#include <stdint.h>


#ifdef __cplusplus
extern "C" {
#endif


/// @brief Allocator
typedef struct pomelo_allocator_s pomelo_allocator_t;

/// @brief Allocation callback
typedef void * (*pomelo_alloc_callback)(void * context, size_t size);

/// @brief Free callback
typedef void (*pomelo_free_callback)(void * context, void * mem);

/// @brief Reallocate callback
typedef void * (*pomelo_realloc_callback)(
    void * context,
    void * mem,
    size_t size
);

/// @brief Allocate failure callback
typedef void (*pomelo_alloc_failure_callback)(void * context, size_t length);


#ifdef __cplusplus

/// Malloc for allocator by type
#define pomelo_allocator_malloc_t(allocator, type)                             \
    static_cast<type*>(pomelo_allocator_malloc(allocator, sizeof(type)))

#else

/// Malloc for allocator by type
#define pomelo_allocator_malloc_t(allocator, type)                             \
    pomelo_allocator_malloc(allocator, sizeof(type))

#endif

/// @brief Allocate new block of memory
void * pomelo_allocator_malloc(pomelo_allocator_t * allocator, size_t size);

/// @brief Free a block of memory
void pomelo_allocator_free(pomelo_allocator_t * allocator, void * mem);

/// @brief Reallocate a block of memory
void * pomelo_allocator_realloc(
    pomelo_allocator_t * allocator,
    void * mem,
    size_t size
);

/// @brief Get the default allocator
pomelo_allocator_t * pomelo_allocator_default(void);

/// @brief Get number of allocated bytes
uint64_t pomelo_allocator_allocated_bytes(pomelo_allocator_t * allocator);

/// @brief Create new allocator
pomelo_allocator_t * pomelo_allocator_create(
    void * context,
    pomelo_alloc_callback alloc_callback,
    pomelo_realloc_callback realloc_callback,
    pomelo_free_callback free_callback
);

/// @brief Destroy an allocator
void pomelo_allocator_destroy(pomelo_allocator_t * allocator);

/// @brief Set allocate failure callback
void pomelo_allocator_set_failure_callback(
    pomelo_allocator_t * allocator,
    pomelo_alloc_failure_callback callback
);

#ifdef __cplusplus
}
#endif
#endif // POMELO_ALLOCATOR_H
