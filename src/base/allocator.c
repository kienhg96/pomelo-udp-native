#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "allocator.h"


#ifndef NDEBUG // Debug mode

/// The signature of all allocators
#define POMELO_ALLOCATOR_SIGNATURE 0x481cfa

/// The signature generator for all elements of an allocator.
/// Zero is reserved for default allocator.
static int element_signature_generator = 0x76a51f;

/// Check the signature of allocator in debug mode
#define pomelo_allocator_check_signature(allocator) \
    assert(allocator->signature == POMELO_ALLOCATOR_SIGNATURE)
#else // !NDEBUG

/// Check the signature of allocator in release mode (No op)
#define pomelo_allocator_check_signature(allocator)

#endif // ~NDEBUG


static void * default_malloc(void * context, size_t size) {
    (void) context;
    return malloc(size);
}

static void default_free(void * context, void * ptr) {
    (void) context;
    free(ptr);
}

static void * default_realloc(void * context, void * ptr, size_t size) {
    (void) context;
    return realloc(ptr, size);
}


/// The default allocator. Do not rely on address of this variable to check if
/// it is default allocator. Use `is_default` instead.
static pomelo_allocator_t default_allocator = {
    .malloc = default_malloc,
    .free = default_free,
    .realloc = default_realloc,
    .is_default = true,
#ifndef NDEBUG
    .signature = POMELO_ALLOCATOR_SIGNATURE,
    .element_signature = 0
#endif // NDEBUG
};


/// @brief Initialize new allocator
static void pomelo_allocator_init(pomelo_allocator_t * allocator) {
    assert(allocator != NULL);
    memset(allocator, 0, sizeof(pomelo_allocator_t));
#ifndef NDEBUG
    allocator->element_signature = ++element_signature_generator;
    allocator->signature = POMELO_ALLOCATOR_SIGNATURE;
#endif

    pomelo_atomic_uint64_store(&allocator->allocated_bytes, 0);
}


pomelo_allocator_t * pomelo_allocator_default(void) {
    return &default_allocator;
}


void * pomelo_allocator_malloc(pomelo_allocator_t * allocator, size_t size) {
    assert(allocator != NULL);
    pomelo_allocator_check_signature(allocator);

    if (size == 0) {
        return NULL;
    }

    void * data = allocator->malloc(
        allocator->context,
        size + sizeof(pomelo_allocator_header_t)
    );

    if (!data) {
        // Failed to allocate
        if (allocator->failure_callback) {
            allocator->failure_callback(allocator->context, size);
        }
        return NULL;
    }

    // Update the header data
    pomelo_allocator_header_t * header = data;
    header->size = size;

#ifndef NDEBUG // Debug mode
    header->signature = allocator->element_signature;
    memset(header + 1, 0xcc, size); // Set dummy value for better debugging
#endif

    // For statistic
    pomelo_atomic_uint64_fetch_add(
        &allocator->allocated_bytes,
        (uint64_t) size
    );

    return header + 1; // Skip the header for payload
}


void pomelo_allocator_free(pomelo_allocator_t * allocator, void * mem) {
    assert(allocator != NULL);
    assert(mem != NULL);
    pomelo_allocator_check_signature(allocator);

    // Point backward to find the header
    pomelo_allocator_header_t * header =
        ((pomelo_allocator_header_t *) mem) - 1;

#ifndef NDEBUG // Debug mode
    assert(header->signature == allocator->element_signature);
#endif

    // For statistic
    pomelo_atomic_uint64_fetch_sub(
        &allocator->allocated_bytes,
        (uint64_t) header->size
    );

    allocator->free(allocator->context, header);
}


void * pomelo_allocator_realloc(
    pomelo_allocator_t * allocator,
    void * mem,
    size_t size
) {
    assert(allocator != NULL);
    pomelo_allocator_check_signature(allocator);
    if (!mem) {
        // Forward to malloc
        return pomelo_allocator_malloc(allocator, size);
    }

    // Point backward to find the header
    pomelo_allocator_header_t * header =
        ((pomelo_allocator_header_t *) mem) - 1;
    size_t prev_size = header->size;

#ifndef NDEBUG // Debug mode
    assert(header->signature == allocator->element_signature);
#endif

    // Realloc new data
    void * data = allocator->realloc(
        allocator->context,
        header,
        size + sizeof(pomelo_allocator_header_t)
    );

    if (!data) {
        return NULL; // Cannot realloc
    }

    // Update the header data
    header = data;
    header->size = size;
    // The rest of header is copied, no need to update

    // Update the allocated bytes
    if (size > prev_size) {
        pomelo_atomic_uint64_fetch_add(
            &allocator->allocated_bytes,
            (uint64_t) (size - prev_size)
        );
    } else if (size < prev_size) {
        pomelo_atomic_uint64_fetch_sub(
            &allocator->allocated_bytes,
            (uint64_t) (prev_size - size)
        );
    }

    return header + 1; // Skip the header for payload
}


uint64_t pomelo_allocator_allocated_bytes(pomelo_allocator_t * allocator) {
    assert(allocator != NULL);
    return pomelo_atomic_uint64_load(&allocator->allocated_bytes);
}


pomelo_allocator_t * pomelo_allocator_create(
    void * context,
    pomelo_alloc_callback alloc_callback,
    pomelo_realloc_callback realloc_callback,
    pomelo_free_callback free_callback
) {
    assert(alloc_callback != NULL);
    assert(free_callback != NULL);

    pomelo_allocator_t * allocator =
        alloc_callback(context, sizeof(pomelo_allocator_t));
    if (!allocator) {
        return NULL;
    }

    pomelo_allocator_init(allocator);
    allocator->malloc = alloc_callback;
    allocator->realloc = realloc_callback;
    allocator->free = free_callback;
    allocator->context = context;

    return allocator;
}


/// @brief Destroy an allocator
void pomelo_allocator_destroy(pomelo_allocator_t * allocator) {
    assert(allocator != NULL);

    pomelo_allocator_check_signature(allocator);
    if (allocator->is_default) {
        return; // Nothing to do with default allocator
    }

    pomelo_free_callback free_fn = allocator->free;
    void * context = allocator->context;
    free_fn(context, allocator);
}


void pomelo_allocator_set_failure_callback(
    pomelo_allocator_t * allocator,
    pomelo_alloc_failure_callback callback
) {
    assert(allocator != NULL);
    allocator->failure_callback = callback;
}


void pomelo_allocator_statistic(
    pomelo_allocator_t * allocator,
    pomelo_statistic_allocator_t * statistic
) {
    assert(allocator != NULL);
    assert(statistic != NULL);

    statistic->allocated_bytes =
        pomelo_atomic_uint64_load(&allocator->allocated_bytes);
}


size_t pomelo_allocator_sizeof(void * ptr) {
    if (!ptr) return 0;
    return (((pomelo_allocator_header_t *) ptr) - 1)->size;
}
