#include "pomelo/allocator.h"
#include "pomelo-test.h"
#include "base-test.h"

static void * test_malloc(void * ctx, size_t size) {
    (void) ctx;
    return malloc(size);
}

static void test_free(void * ctx, void * ptr) {
    (void) ctx;
    free(ptr);
}

static void * test_realloc(void * ctx, void * ptr, size_t size) {
    (void) ctx;
    return realloc(ptr, size);
}

int pomelo_test_allocator(void) {
    // Test default allocator
    pomelo_allocator_t * allocator = pomelo_allocator_default();

    pomelo_check(allocator != NULL);
    void * mem = pomelo_allocator_malloc(allocator, 128);
    pomelo_check(pomelo_allocator_allocated_bytes(allocator) == 128);
    pomelo_check(mem != NULL);
    pomelo_allocator_free(allocator, mem);
    pomelo_check(pomelo_allocator_allocated_bytes(allocator) == 0);

    // Test custom allocator
    pomelo_allocator_t * custom_allocator = pomelo_allocator_create(
        NULL,           // context
        test_malloc,    // alloc callback
        test_realloc,   // realloc callback
        test_free       // free callback
    );
    
    pomelo_check(custom_allocator != NULL);
    void * custom_mem = pomelo_allocator_malloc(custom_allocator, 256);
    pomelo_check(custom_mem != NULL);
    pomelo_check(pomelo_allocator_allocated_bytes(custom_allocator) == 256);
    pomelo_allocator_free(custom_allocator, custom_mem);
    pomelo_check(pomelo_allocator_allocated_bytes(custom_allocator) == 0);
    
    pomelo_allocator_destroy(custom_allocator);

    return 0;
}
