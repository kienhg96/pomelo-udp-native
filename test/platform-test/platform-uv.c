#include "platform-test.h"
#include "pomelo/platforms/platform-uv.h"
#include "pomelo-test.h"


/* Use default loop of UV */

pomelo_platform_t * pomelo_test_platform_create(
    pomelo_allocator_t * allocator
) {
    // Create platform first
    pomelo_platform_uv_options_t options = {
        .allocator = allocator,
        .uv_loop = uv_default_loop()
    };

    return pomelo_platform_uv_create(&options);
}


void pomelo_test_platform_destroy(pomelo_platform_t * platform) {
    pomelo_platform_uv_destroy(platform);
}


void pomelo_test_platform_run(pomelo_platform_t * platform) {
    (void) platform;
    uv_run(uv_default_loop(), UV_RUN_DEFAULT);
    // All platform handles (including idle_close) must be closed so the loop
    // can be torn down cleanly.
    pomelo_check(uv_loop_close(uv_default_loop()) == 0);
}
