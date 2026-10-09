#include <assert.h>
#include <string.h>
#include "context.h"
#include "message.h"
#include "socket.h"
#include "base/allocator.h"
#include "base/constants.h"
#include "base/ref.h"
#include "delivery/parcel.h"
#include "delivery/context.h"
#include "plugin/manager.h"
#include "utils/macro.h"
#include "crypto/crypto.h"
#include "builtin/session.h"
#include "builtin/channel.h"
#include "plugin/session.h"
#include "plugin/channel.h"


/// The capacity of fragment body
#define POMELO_FRAGMENT_BODY_CAPACITY                                          \
    (POMELO_PACKET_BODY_CAPACITY - POMELO_MAX_FRAGMENT_META_DATA_BYTES)


/* -------------------------------------------------------------------------- */
/*                             Root context APIs                              */
/* -------------------------------------------------------------------------- */


pomelo_context_t * pomelo_context_root_create(
    pomelo_context_root_options_t * options
) {
    assert(options != NULL);
    if (pomelo_crypto_init() < 0) {
        return NULL; // Failed to initialize codec
    }

    pomelo_allocator_t * allocator = options->allocator;
    if (!allocator) {
        allocator = pomelo_allocator_default();
    }

    size_t message_capacity = options->message_capacity;
    if (message_capacity == 0) {
        message_capacity = POMELO_MESSAGE_DEFAULT_CAPACITY;
    }

    pomelo_context_root_t * context =
        pomelo_allocator_malloc_t(allocator, pomelo_context_root_t);
    if (!context) return NULL;
    memset(context, 0, sizeof(pomelo_context_root_t));
    pomelo_context_t * base = &context->base;

    context->allocator = allocator;

    // Create buffer context
    pomelo_buffer_context_root_options_t buffer_context_options = {
        .allocator = allocator,
        .buffer_capacity = POMELO_BUFFER_CAPACITY,
        .synchronized = options->synchronized
    };
    context->buffer_context =
        pomelo_buffer_context_root_create(&buffer_context_options);
    if (!context->buffer_context) {
        pomelo_context_root_destroy(context);
        return NULL;
    }

    // Create delivery context
    pomelo_delivery_context_root_options_t delivery_context_options = {
        .allocator = allocator,
        .buffer_context = context->buffer_context,
        .fragment_capacity = POMELO_PACKET_BODY_CAPACITY,
        .max_fragments = POMELO_CEIL_DIV(
            message_capacity,
            POMELO_FRAGMENT_BODY_CAPACITY
        ),
        .synchronized = options->synchronized
    };
    context->delivery_context =
        pomelo_delivery_context_root_create(&delivery_context_options);
    if (!context->delivery_context) {
        pomelo_context_root_destroy(context);
        return NULL;
    }

    // Create protocol context
    pomelo_protocol_context_options_t protocol_context_options = {
        .allocator = allocator,
        .buffer_context = context->buffer_context,
        .payload_capacity = POMELO_PACKET_BODY_CAPACITY
    };
    base->protocol_context =
        pomelo_protocol_context_create(&protocol_context_options);
    if (!base->protocol_context) {
        pomelo_context_root_destroy(context);
        return NULL;
    }

    // Create message pool
    pomelo_pool_root_options_t pool_options;
    memset(&pool_options, 0, sizeof(pomelo_pool_root_options_t));
    pool_options.allocator = allocator;
    pool_options.element_size = sizeof(pomelo_message_t);
    pool_options.zero_init = true;
    pool_options.on_init = (pomelo_pool_init_cb) pomelo_message_init;
    pool_options.on_cleanup = (pomelo_pool_cleanup_cb) pomelo_message_cleanup;
    pool_options.synchronized = options->synchronized;

    context->message_pool = pomelo_pool_root_create(&pool_options);
    if (!context->message_pool) {
        pomelo_context_root_destroy(context);
        return NULL;
    }

    // Setup the plugin manager
    pomelo_plugin_manager_options_t plugin_manager_options = {
        .allocator = allocator
    };
    pomelo_plugin_manager_t * plugin_manager =
        pomelo_plugin_manager_create(&plugin_manager_options);
    if (!plugin_manager) {
        pomelo_context_root_destroy(context);
        return NULL;
    }
    context->plugin_manager = plugin_manager;

    // Create socket pool
    memset(&pool_options, 0, sizeof(pomelo_pool_root_options_t));
    pool_options.allocator = allocator;
    pool_options.element_size = sizeof(pomelo_socket_t);
    pool_options.on_alloc = (pomelo_pool_alloc_cb) pomelo_socket_on_alloc;
    pool_options.on_free = (pomelo_pool_free_cb) pomelo_socket_on_free;
    pool_options.on_init = (pomelo_pool_init_cb) pomelo_socket_init;
    pool_options.on_cleanup = (pomelo_pool_cleanup_cb) pomelo_socket_cleanup;
    pool_options.alloc_data = context;
    base->socket_pool = pomelo_pool_root_create(&pool_options);
    if (!base->socket_pool) {
        pomelo_context_root_destroy(context);
        return NULL;
    }

    // Create builtin session pool
    memset(&pool_options, 0, sizeof(pomelo_pool_root_options_t));
    pool_options.allocator = allocator;
    pool_options.element_size = sizeof(pomelo_session_builtin_t);
    pool_options.on_alloc = (pomelo_pool_alloc_cb)
        pomelo_session_builtin_on_alloc;
    pool_options.on_free = (pomelo_pool_free_cb)
        pomelo_session_builtin_on_free;
    pool_options.on_init = (pomelo_pool_init_cb) pomelo_session_builtin_init;
    pool_options.on_cleanup = (pomelo_pool_cleanup_cb)
        pomelo_session_builtin_cleanup;
    pool_options.alloc_data = context;

    base->builtin_session_pool = pomelo_pool_root_create(&pool_options);
    if (!base->builtin_session_pool) {
        pomelo_context_root_destroy(context);
        return NULL;
    }

    // Create builtin channel pool
    memset(&pool_options, 0, sizeof(pomelo_pool_root_options_t));
    pool_options.allocator = allocator;
    pool_options.element_size = sizeof(pomelo_channel_builtin_t);
    pool_options.alloc_data = context;
    pool_options.zero_init = true;
    pool_options.on_init = (pomelo_pool_init_cb) pomelo_channel_builtin_init;
    pool_options.on_cleanup = (pomelo_pool_cleanup_cb)
        pomelo_channel_builtin_cleanup;
    base->builtin_channel_pool = pomelo_pool_root_create(&pool_options);
    if (!base->builtin_channel_pool) {
        pomelo_context_root_destroy(context);
        return NULL;
    }

    // Create plugin session pool
    memset(&pool_options, 0, sizeof(pomelo_pool_root_options_t));
    pool_options.allocator = allocator;
    pool_options.element_size = sizeof(pomelo_session_plugin_t);
    pool_options.alloc_data = context;
    pool_options.on_alloc = (pomelo_pool_alloc_cb)
        pomelo_session_plugin_on_alloc;
    pool_options.on_free = (pomelo_pool_free_cb)
        pomelo_session_plugin_on_free;
    pool_options.on_init = (pomelo_pool_init_cb) pomelo_session_plugin_init;
    pool_options.on_cleanup = (pomelo_pool_cleanup_cb)
        pomelo_session_plugin_cleanup;
    base->plugin_session_pool = pomelo_pool_root_create(&pool_options);
    if (!base->plugin_session_pool) {
        pomelo_context_root_destroy(context);
        return NULL;
    }
    
    // Create plugin channel pool
    memset(&pool_options, 0, sizeof(pomelo_pool_root_options_t));
    pool_options.allocator = allocator;
    pool_options.element_size = sizeof(pomelo_channel_plugin_t);
    pool_options.alloc_data = context;
    pool_options.zero_init = true;
    pool_options.on_init = (pomelo_pool_init_cb) pomelo_channel_plugin_init;
    pool_options.on_cleanup = (pomelo_pool_cleanup_cb)
        pomelo_channel_plugin_cleanup;
    base->plugin_channel_pool = pomelo_pool_root_create(&pool_options);
    if (!base->plugin_channel_pool) {
        pomelo_context_root_destroy(context);
        return NULL;
    }

    // Setup the interface
    base->root = context; // Root is itself
    pomelo_extra_set(base->extra, NULL);
    base->allocator = allocator;
    base->acquire_message = (pomelo_context_acquire_message_fn)
        pomelo_context_root_acquire_message;
    base->release_message = (pomelo_context_release_message_fn)
        pomelo_context_root_release_message;
    base->statistic = (pomelo_context_statistic_fn)
        pomelo_context_root_statistic;
    base->buffer_context = (pomelo_buffer_context_t *)
        context->buffer_context;
    base->delivery_context = (pomelo_delivery_context_t *)
        context->delivery_context;
    base->plugin_manager = plugin_manager;
    base->message_capacity = message_capacity;

    return base;
}


void pomelo_context_root_destroy(pomelo_context_root_t * context) {
    assert(context != NULL);
    pomelo_context_t * base = &context->base;

    // Destroy order matters when the context is torn down abruptly while a
    // session still has an in-flight (reliable, not-yet-ACKed) message, e.g. a
    // binding calling pomelo_context_destroy without a graceful socket stop.
    //
    // Destroying a session pool runs pomelo_session_*_cleanup synchronously,
    // which stops the delivery endpoint; the stop cancels the parked reliable
    // dispatcher, which still reports a send result back through
    // pomelo_socket_dispatch_send_result -> pomelo_message_ref() on the message
    // and dereferences the owning socket. So the session (and channel) pools
    // must be destroyed BEFORE socket_pool and message_pool, while the socket
    // and message memory are still alive; otherwise the flush touches freed
    // objects ("Try to ref finalized reference", ref.c). Session cleanup also
    // releases channels back to the channel pools, so sessions come before
    // channels. See REVIEW.md section 6 (N2).
    if (base->builtin_session_pool) {
        pomelo_pool_destroy(base->builtin_session_pool);
        base->builtin_session_pool = NULL;
    }

    if (base->plugin_session_pool) {
        pomelo_pool_destroy(base->plugin_session_pool);
        base->plugin_session_pool = NULL;
    }

    if (base->builtin_channel_pool) {
        pomelo_pool_destroy(base->builtin_channel_pool);
        base->builtin_channel_pool = NULL;
    }

    if (base->plugin_channel_pool) {
        pomelo_pool_destroy(base->plugin_channel_pool);
        base->plugin_channel_pool = NULL;
    }

    if (base->socket_pool) {
        pomelo_pool_destroy(base->socket_pool);
        base->socket_pool = NULL;
    }

    if (context->message_pool) {
        pomelo_pool_destroy(context->message_pool);
        context->message_pool = NULL;
    }

    if (base->protocol_context) {
        pomelo_protocol_context_destroy(base->protocol_context);
        base->protocol_context = NULL;
    }

    if (context->delivery_context) {
        pomelo_delivery_context_destroy(context->delivery_context);
        context->delivery_context = NULL;
    }

    if (context->buffer_context) {
        pomelo_buffer_context_destroy(context->buffer_context);
        context->buffer_context = NULL;
    }

    if (context->plugin_manager) {
        pomelo_plugin_manager_destroy(context->plugin_manager);
        context->plugin_manager = NULL;
    }

    pomelo_allocator_free(context->allocator, context);
}


pomelo_message_t * pomelo_context_root_acquire_message(
    pomelo_context_root_t * context,
    pomelo_message_info_t * info
) {
    assert(context != NULL);
    return pomelo_pool_acquire(context->message_pool, info);
}


void pomelo_context_root_release_message(
    pomelo_context_root_t * context,
    pomelo_message_t * message
) {
    assert(context != NULL);
    assert(message->context == &context->base);
    pomelo_pool_release(context->message_pool, message);
}


void pomelo_context_root_statistic(
    pomelo_context_root_t * context,
    pomelo_statistic_api_t * statistic
) {
    assert(context != NULL);
    assert(statistic != NULL);

    statistic->messages = pomelo_pool_in_use(context->message_pool);

    pomelo_context_t * base = &context->base;
    statistic->builtin_sessions = pomelo_pool_in_use(
        base->builtin_session_pool
    );
    statistic->plugin_sessions = pomelo_pool_in_use(
        base->plugin_session_pool
    );
    statistic->builtin_channels = pomelo_pool_in_use(
        base->builtin_channel_pool
    );
    statistic->plugin_channels = pomelo_pool_in_use(
        base->plugin_channel_pool
    );
}


/* -------------------------------------------------------------------------- */
/*                            Shared context APIs                             */
/* -------------------------------------------------------------------------- */

pomelo_context_t * pomelo_context_shared_create(
    pomelo_context_shared_options_t * options
) {
    assert(options != NULL);
    if (!options->context) {
        return NULL;
    }

    pomelo_allocator_t * allocator = options->allocator;
    if (!allocator) {
        allocator = pomelo_allocator_default();
    }

    pomelo_context_shared_t * context =
        pomelo_allocator_malloc_t(allocator, pomelo_context_shared_t);
    if (!context) return NULL;

    pomelo_context_root_t * root = options->context->root;
    memset(context, 0, sizeof(pomelo_context_shared_t));
    context->allocator = allocator;

    // Create buffer context
    pomelo_buffer_context_shared_options_t buffer_context_options = {
        .allocator = allocator,
        .context = root->buffer_context
    };
    context->buffer_context =
        pomelo_buffer_context_shared_create(&buffer_context_options);
    if (!context->buffer_context) {
        // Failed to create new buffer context
        pomelo_context_shared_destroy(context);
        return NULL;
    }

    // Create local-thread delivery context
    pomelo_delivery_context_shared_options_t delivery_context_options = {
        .allocator = allocator,
        .origin_context = root->delivery_context
    };
    context->delivery_context = pomelo_delivery_context_shared_create(
        &delivery_context_options
    );
    if (!context->delivery_context) {
        // Failed to create new delivery context
        pomelo_context_shared_destroy(context);
        return NULL;
    }

    // Create shared messages pool
    pomelo_pool_shared_options_t pool_options = {
        .allocator = allocator,
        .buffers = POMELO_API_MESSAGES_POOL_BUFFER_SHARED_BUFFER_SIZE,
        .origin_pool = root->message_pool
    };
    context->message_pool = pomelo_pool_shared_create(&pool_options);
    if (!context->message_pool) {
        // Failed to create message pool
        pomelo_context_shared_destroy(context);
        return NULL;
    }

    // Setup the interface
    pomelo_context_t * base = &context->base;
    base->root = root;
    base->allocator = allocator;
    pomelo_extra_set(base->extra, NULL);
    base->acquire_message = (pomelo_context_acquire_message_fn)
        pomelo_context_shared_acquire_message;
    base->release_message = (pomelo_context_release_message_fn)
        pomelo_context_shared_release_message;
    base->statistic = (pomelo_context_statistic_fn)
        pomelo_context_shared_statistic;
    base->buffer_context = (pomelo_buffer_context_t *)
        context->buffer_context;
    base->protocol_context = (pomelo_protocol_context_t *)
        context->base.protocol_context;
    base->delivery_context = (pomelo_delivery_context_t *)
        context->delivery_context;
    base->plugin_manager = root->plugin_manager;
    base->message_capacity = root->base.message_capacity;
    base->socket_pool = root->base.socket_pool;
    base->builtin_session_pool = root->base.builtin_session_pool;
    base->builtin_channel_pool = root->base.builtin_channel_pool;
    base->plugin_session_pool = root->base.plugin_session_pool;
    base->plugin_channel_pool = root->base.plugin_channel_pool;

    return base;
}


void pomelo_context_shared_destroy(pomelo_context_shared_t * context) {
    assert(context != NULL);

    if (context->delivery_context) {
        pomelo_delivery_context_destroy(context->delivery_context);
        context->delivery_context = NULL;
    }

    if (context->message_pool) {
        pomelo_pool_destroy(context->message_pool);
        context->message_pool = NULL;
    }

    if (context->buffer_context) {
        pomelo_buffer_context_destroy(context->buffer_context);
        context->buffer_context = NULL;
    }

    pomelo_allocator_free(context->allocator, context);
}


pomelo_message_t * pomelo_context_shared_acquire_message(
    pomelo_context_shared_t * context,
    pomelo_message_info_t * info
) {
    assert(context != NULL);
    return pomelo_pool_acquire(context->message_pool, info);
}


void pomelo_context_shared_release_message(
    pomelo_context_shared_t * context,
    pomelo_message_t * message
) {
    assert(context != NULL);
    assert(message->context == &context->base);
    pomelo_pool_release(context->message_pool, message);
}


void pomelo_context_shared_statistic(
    pomelo_context_shared_t * context,
    pomelo_statistic_api_t * statistic
) {
    assert(context != NULL);
    pomelo_context_root_statistic(context->base.root, statistic);
}


void pomelo_context_destroy(pomelo_context_t * context) {
    assert(context != NULL);
    if ((pomelo_context_t *) context->root == context) {
        pomelo_context_root_destroy((pomelo_context_root_t *) context);
    } else {
        pomelo_context_shared_destroy((pomelo_context_shared_t *) context);
    }
}


void pomelo_context_set_extra(pomelo_context_t * context, void * data) {
    assert(context != NULL);
    pomelo_extra_set(context->extra, data);
}


void * pomelo_context_get_extra(pomelo_context_t * context) {
    assert(context != NULL);
    return pomelo_extra_get(context->extra);
}


pomelo_message_t * pomelo_context_acquire_message(pomelo_context_t * context) {
    assert(context != NULL);
    pomelo_delivery_parcel_t * parcel =
        pomelo_delivery_context_acquire_parcel(context->delivery_context);
    if (!parcel) return NULL; // Failed to acquire parcel

    // Acquire new message and attach the parcel to message
    pomelo_message_info_t info = {
        .context = context,
        .mode = POMELO_MESSAGE_MODE_WRITE,
        .parcel = parcel
    };
    pomelo_message_t * message =
        pomelo_context_acquire_message_ex(context, &info);
    
    // Unref the parcel
    pomelo_delivery_parcel_unref(parcel);
    return message;
}


pomelo_message_t * pomelo_context_acquire_message_ex(
    pomelo_context_t * context,
    pomelo_message_info_t * info
) {
    assert(context != NULL);
    return context->acquire_message(context, info);
}


void pomelo_context_release_message(
    pomelo_context_t * context,
    pomelo_message_t * message
) {
    assert(context != NULL);
    assert(message->context == context);
    context->release_message(context, message);
}


void pomelo_context_statistic(
    pomelo_context_t * context,
    pomelo_statistic_t * statistic
) {
    assert(statistic != NULL);
    assert(context != NULL);
    memset(statistic, 0, sizeof(pomelo_statistic_t));

    // API statistic
    context->statistic(context, &statistic->api);

    // Allocator statistic
    pomelo_allocator_statistic(context->allocator, &statistic->allocator);

    // Buffer context statistic
    pomelo_buffer_context_statistic(
        context->buffer_context,
        &statistic->buffer
    );

    // Statistic of protocol
    pomelo_protocol_context_statistic(
        context->protocol_context,
        &statistic->protocol
    );

    // Statistic of delivery
    pomelo_delivery_context_statistic(
        context->delivery_context,
        &statistic->delivery
    );
}
