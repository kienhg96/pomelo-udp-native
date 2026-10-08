#include <assert.h>
#include <string.h>
#include "pomelo/random.h"
#include "sodium/randombytes.h"


void pomelo_random_buffer(void * buffer, size_t length) {
    assert(buffer != NULL);
    randombytes_buf(buffer, length);
}


void pomelo_random_buffer_deterministic(
    void * buffer,
    size_t length,
    uint64_t seed
) {
    assert(buffer != NULL);
    if (length == 0) return; // Empty

    // Serialize the full 64-bit seed (big-endian) into a fully-zeroed
    // seed array of randombytes_SEEDBYTES bytes.
    uint8_t seed_arr[randombytes_SEEDBYTES];
    memset(seed_arr, 0, sizeof(seed_arr));
    for (int i = 0; i < 8; ++i) {
        seed_arr[i] = (uint8_t) ((seed >> (56 - i * 8)) & 0xFF);
    }

    randombytes_buf_deterministic(buffer, length, seed_arr);
}
