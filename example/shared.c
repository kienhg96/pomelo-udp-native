#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include "shared.h"


static pomelo_channel_mode CHANNEL_MODES[] = {
    POMELO_CHANNEL_MODE_RELIABLE,
    POMELO_CHANNEL_MODE_SEQUENCED,
    POMELO_CHANNEL_MODE_UNRELIABLE
};


void pomelo_example_env_init(
    pomelo_example_env_t * env,
    const char * plugin_path
) {
    assert(env != NULL);

    pomelo_allocator_t * allocator = pomelo_allocator_default();
    allocator = pomelo_allocator_default();
    env->allocator = allocator;
    env->allocator_bytes_begin = pomelo_allocator_allocated_bytes(allocator);

    // Create platform
    uv_loop_t * loop = &env->loop;
    uv_loop_init(loop);
    pomelo_platform_uv_options_t platform_options = {
        .allocator = allocator,
        .uv_loop = loop
    };
    pomelo_platform_t * platform = pomelo_platform_uv_create(&platform_options);
    assert(platform != NULL);
    env->platform = platform;

    // Create context
    pomelo_context_root_options_t context_options = {
        .allocator = allocator
    };
    pomelo_context_t * context = pomelo_context_root_create(&context_options);
    assert(context != NULL);
    env->context = context;

    // Register plugin
    if (plugin_path) {
        pomelo_plugin_initializer initializer =
            pomelo_plugin_load_by_path(plugin_path);
        if (initializer) {
            pomelo_plugin_t * plugin = pomelo_plugin_register(
                allocator,
                context,
                platform,
                initializer
            );
            if (!plugin) {
                printf("Failed to initialize plugin: %s\n", plugin_path);
            }
        } else {
            printf("Failed to load plugin: %s\n", plugin_path);
        }
    }

    // Create socket
    pomelo_socket_options_t options = {
        .channel_modes = CHANNEL_MODES,
        .context = env->context,
        .nchannels = sizeof(CHANNEL_MODES) / sizeof(CHANNEL_MODES[0]),
        .platform = env->platform
    };

    pomelo_socket_t * socket = pomelo_socket_create(&options);
    assert(socket != NULL);
    env->socket = socket;
}


void pomelo_example_env_finalize(pomelo_example_env_t * env) {
    pomelo_allocator_t * allocator = env->allocator;
    pomelo_context_t * context = env->context;
    pomelo_platform_t * platform = env->platform;
    uint64_t allocated_bytes = env->allocator_bytes_begin;

    // Destroy socket
    pomelo_socket_destroy(env->socket);

    // Destroy the context
    pomelo_context_destroy(context);
    
    // Destroy the platform
    pomelo_platform_uv_destroy(platform);

    // Close UV loop
    uv_loop_close(&env->loop);

    // Check memleak
    pomelo_assert(
        pomelo_allocator_allocated_bytes(allocator) == allocated_bytes
    );
}
