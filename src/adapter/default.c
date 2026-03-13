#include <assert.h>
#include <string.h>
#include "default.h"


pomelo_adapter_t * pomelo_adapter_create(pomelo_adapter_options_t * options) {
    assert(options != NULL);
    if (!options->platform) {
        return NULL;
    }

    pomelo_allocator_t * allocator = options->allocator;
    if (!allocator) {
        allocator = pomelo_allocator_default();
    }

    pomelo_adapter_t * adapter =
        pomelo_allocator_malloc_t(allocator, pomelo_adapter_t);
    if (!adapter) {
        return NULL;
    }
    memset(adapter, 0, sizeof(pomelo_adapter_t));

    adapter->allocator = allocator;
    adapter->platform = options->platform;

    return adapter;
}


void pomelo_adapter_destroy(pomelo_adapter_t * adapter) {
    assert(adapter != NULL);
    pomelo_allocator_free(adapter->allocator, adapter);
}


void pomelo_adapter_set_extra(pomelo_adapter_t * adapter, void * extra) {
    assert(adapter != NULL);
    adapter->extra = extra;
}


void * pomelo_adapter_get_extra(pomelo_adapter_t * adapter) {
    assert(adapter != NULL);
    return adapter->extra;
}


uint32_t pomelo_adapter_get_capability(pomelo_adapter_t * adapter) {
    (void) adapter;

#ifdef POMELO_ADAPTER_DEFAULT_NO_ENCRYPTION
    // This adapter only supports unencrypted packets
    return (
        POMELO_ADAPTER_CAPABILITY_SERVER_UNENCRYPTED |
        POMELO_ADAPTER_CAPABILITY_CLIENT_UNENCRYPTED
    );
#else
    // This adapter only supports encrypted packets
    return (
        POMELO_ADAPTER_CAPABILITY_SERVER_ENCRYPTED |
        POMELO_ADAPTER_CAPABILITY_CLIENT_ENCRYPTED
    );
#endif
}


int pomelo_adapter_connect(
    pomelo_adapter_t * adapter,
    pomelo_address_t * address
) {
    assert(adapter != NULL);
    assert(address != NULL);
    if (adapter->flags & POMELO_ADAPTER_FLAG_STARTED) {
        return -1; // Already started
    }

    int ret = pomelo_platform_udp_connect(
        adapter->platform,
        address,
        &adapter->udp
    );
    if (ret < 0) {
        return ret; // Failed to start connecting
    }

    adapter->flags |= POMELO_ADAPTER_FLAG_CONNECTING;
    pomelo_adapter_recv_start(adapter);
    return 0;
}


int pomelo_adapter_listen(
    pomelo_adapter_t * adapter,
    pomelo_address_t * address
) {
    assert(adapter != NULL);
    assert(address != NULL);
    if (adapter->flags & POMELO_ADAPTER_FLAG_STARTED) {
        return -1; // Already started
    }

    int ret = pomelo_platform_udp_bind(
        adapter->platform,
        address,
        &adapter->udp
    );
    if (ret < 0) {
        return ret; // Failed to start listening
    }

    adapter->flags |= POMELO_ADAPTER_FLAG_LISTENING;
    pomelo_adapter_recv_start(adapter);
    return 0;
}


int pomelo_adapter_stop(pomelo_adapter_t * adapter) {
    assert(adapter != NULL);
    if (!(adapter->flags & POMELO_ADAPTER_FLAG_STARTED)) {
        return -1; // Not started
    }

    pomelo_platform_udp_stop(adapter->platform, &adapter->udp);
    adapter->flags &= ~POMELO_ADAPTER_FLAG_STARTED;
    return 0;
}


/// @brief Process the send complete event.
static void process_send_complete(pomelo_buffer_t * buffer, int status) {
    assert(buffer != NULL);
    (void) status;

    // Unref the buffer
    pomelo_buffer_unref(buffer);
}


int pomelo_adapter_send(
    pomelo_adapter_t * adapter,
    pomelo_address_t * address,
    pomelo_buffer_view_t * view,
    bool encrypted
) {
    assert(adapter != NULL);
    assert(view != NULL);
    // Make sure the adapter is running
    if (!(adapter->flags & POMELO_ADAPTER_FLAG_STARTED)) return -1;

#ifdef POMELO_ADAPTER_DEFAULT_NO_ENCRYPTION
    // Default adapter requires all packets must be unencrypted.
    if (encrypted) return -1;
#else
    // Default adapter requires all packets must be encrypted.
    if (!encrypted) return -1;
#endif

    // Ref the buffer
    pomelo_buffer_t * buffer = view->buffer;
    pomelo_buffer_ref(buffer);

    // Build vector
    pomelo_platform_iovec_t vec;
    vec.data = buffer->data + view->offset;
    vec.length = view->length;

    int ret = pomelo_platform_udp_send(
        adapter->platform,
        &adapter->udp,
        address,
        1,
        &vec,
        buffer,
        (pomelo_platform_send_cb) process_send_complete
    );
    if (ret < 0) {
        pomelo_buffer_unref(buffer);
        return -1;
    }

    return 0;
}


void pomelo_adapter_alloc_callback(
    pomelo_adapter_t * adapter,
    pomelo_platform_iovec_t * iovec
) {
    assert(adapter != NULL);
    assert(iovec != NULL);
    if (!(adapter->flags & POMELO_ADAPTER_FLAG_STARTED)) {
        return; // Make sure the adapter is running
    }

    pomelo_buffer_t * buffer = pomelo_adapter_buffer_acquire(adapter);
    if (!buffer) return;

    iovec->data = buffer->data;
    iovec->length = buffer->capacity;
}


void pomelo_adapter_recv_callback(
    pomelo_adapter_t * adapter,
    pomelo_address_t * address,
    pomelo_platform_iovec_t * iovec,
    int status
) {
    assert(adapter != NULL);
    assert(iovec != NULL);
    if (!(adapter->flags & POMELO_ADAPTER_FLAG_STARTED)) {
        return; // Make sure the adapter is running
    }

    pomelo_buffer_t * buffer = pomelo_buffer_from_data(iovec->data);
    if (status == 0) {
        pomelo_buffer_view_t view;
        view.buffer = buffer;
        view.offset = 0;
        view.length = iovec->length;
#ifdef POMELO_ADAPTER_DEFAULT_NO_ENCRYPTION
        pomelo_adapter_on_recv(adapter, address, &view, false);
#else
        pomelo_adapter_on_recv(adapter, address, &view, true);
#endif
    }

    pomelo_buffer_unref(buffer);
}


void pomelo_adapter_send_callback(
    void * context,
    pomelo_buffer_t * buffer,
    int status
) {
    // Warn: The context here may be destroyed, so don't use it.
    (void) context;
    (void) status;

    // Unref the buffer
    pomelo_buffer_unref(buffer);
}


void pomelo_adapter_recv_start(pomelo_adapter_t * adapter) {
    assert(adapter != NULL);

    // Start receiving
    pomelo_platform_udp_recv_start(
        adapter->platform,
        &adapter->udp,
        adapter, // context
        (pomelo_platform_alloc_cb) pomelo_adapter_alloc_callback,
        (pomelo_platform_recv_cb) pomelo_adapter_recv_callback
    );
}
