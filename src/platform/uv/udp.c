#include <assert.h>
#include <stdbool.h>
#include "base/constants.h"
#include "udp.h"


/* -------------------------------------------------------------------------- */
/*                                Public APIs                                 */
/* -------------------------------------------------------------------------- */

pomelo_platform_uv_udp_mgr_t * pomelo_platform_uv_udp_mgr_create(
    pomelo_platform_uv_t * platform,
    pomelo_allocator_t * allocator,
    uv_loop_t * uv_loop
) {
    assert(allocator != NULL);
    assert(uv_loop != NULL);

    pomelo_platform_uv_udp_mgr_t * manager = pomelo_allocator_malloc_t(
        allocator,
        pomelo_platform_uv_udp_mgr_t
    );

    if (!manager) {
        return NULL;
    }

    memset(manager, 0, sizeof(pomelo_platform_uv_udp_mgr_t));
    manager->platform = platform;
    manager->allocator = allocator;
    manager->uv_loop = uv_loop;

    // Create send object pool
    pomelo_pool_root_options_t pool_options;
    memset(&pool_options, 0, sizeof(pomelo_pool_root_options_t));
    pool_options.allocator = allocator;
    pool_options.element_size = sizeof(pomelo_platform_uv_send_t);

    manager->send_pool = pomelo_pool_root_create(&pool_options);
    if (!manager->send_pool) {
        pomelo_platform_uv_udp_mgr_destroy(manager);
        return NULL;
    }

    // Create pool of sockets
    memset(&pool_options, 0, sizeof(pomelo_pool_root_options_t));
    pool_options.allocator = allocator;
    pool_options.element_size = sizeof(pomelo_platform_uv_udp_t);
    pool_options.zero_init = true;

    manager->socket_pool = pomelo_pool_root_create(&pool_options);
    if (!manager->socket_pool) {
        pomelo_platform_uv_udp_mgr_destroy(manager);
        return NULL;
    }

    // Create list of running sockets
    pomelo_list_options_t list_options = {
        .allocator = allocator,
        .element_size = sizeof(pomelo_platform_uv_udp_t *)
    };
    manager->sockets = pomelo_list_create(&list_options);
    if (!manager->sockets) {
        pomelo_platform_uv_udp_mgr_destroy(manager);
        return NULL;
    }

    return manager;
}


void pomelo_platform_uv_udp_mgr_destroy(
    pomelo_platform_uv_udp_mgr_t * manager
) {
    assert(manager != NULL);
    pomelo_allocator_t * allocator = manager->allocator;

    if (manager->send_pool) {
        pomelo_pool_destroy(manager->send_pool);
        manager->send_pool = NULL;
    }

    if (manager->socket_pool) {
        pomelo_pool_destroy(manager->socket_pool);
        manager->socket_pool = NULL;
    }

    if (manager->sockets) {
        pomelo_list_destroy(manager->sockets);
        manager->sockets = NULL;
    }

    pomelo_allocator_free(allocator, manager);
}


void pomelo_platform_uv_udp_mgr_statistic(
    pomelo_platform_uv_udp_mgr_t * manager,
    pomelo_statistic_platform_uv_t * statistic
) {
    assert(manager != NULL);
    assert(statistic != NULL);

    statistic->send_commands = pomelo_pool_in_use(manager->send_pool);
    statistic->sent_bytes = manager->send_bytes;
    statistic->recv_bytes = manager->recv_bytes;
}


void pomelo_platform_uv_udp_mgr_close(pomelo_platform_uv_udp_mgr_t * manager) {
    assert(manager != NULL);
    pomelo_platform_uv_udp_t * socket = NULL;
    pomelo_list_iterator_t it;
    pomelo_list_iterator_init(&it, manager->sockets);
    while (pomelo_list_iterator_next(&it, &socket) == 0) {
        pomelo_platform_uv_udp_close(socket);
    }

    pomelo_platform_uv_udp_mgr_check_closed(manager);
}


int pomelo_platform_uv_udp_bind(
    pomelo_platform_uv_t * platform,
    pomelo_address_t * address,
    pomelo_platform_handle_t * udp_handle // Might be NULL
) {
    assert(platform != NULL);
    assert(address != NULL);

    pomelo_platform_uv_udp_mgr_t * manager = platform->udp_mgr;

    // Convert address to sockaddr
    struct sockaddr_storage addr;
    if (pomelo_address_to_sockaddr(address, &addr) < 0) return -1;

    // Acquire socket
    pomelo_platform_uv_udp_t * socket =
        pomelo_pool_acquire(manager->socket_pool, NULL);
    if (!socket) return -1; // Failed to acquire socket

    // Setup socket
    socket->manager = manager;

    // Setup UDP handle
    uv_udp_t * udp = &socket->uv_udp;
    uv_udp_init(manager->uv_loop, udp);
    udp->data = socket;

    int send_buf_size = POMELO_SERVER_SOCKET_SNDBUF_SIZE;
    int recv_buf_size = POMELO_SERVER_SOCKET_RCVBUF_SIZE;

    if (uv_udp_bind(udp, (struct sockaddr *) &addr, UV_UDP_REUSEADDR) < 0) {
        pomelo_platform_uv_udp_close(socket);
        return -1;
    }

    uv_send_buffer_size((uv_handle_t *) udp, &send_buf_size);
    uv_recv_buffer_size((uv_handle_t *) udp, &recv_buf_size);

    int ret = uv_udp_recv_start(
        udp,
        pomelo_platform_uv_udp_alloc_callback,
        pomelo_platform_uv_udp_recv_callback
    );
    if (ret < 0) {
        pomelo_platform_uv_udp_close(socket);
        return -1;
    }

    // Push socket to list
    socket->entry = pomelo_list_push_back(manager->sockets, socket);
    if (!socket->entry) {
        pomelo_platform_uv_udp_close(socket);
        return -1;
    }

    // Attach the socket to the handle
    socket->handle = udp_handle;
    if (udp_handle) {
        udp_handle->type = POMELO_PLATFORM_HANDLE_UDP;
        udp_handle->data = socket;
    }

    return 0;
}


int pomelo_platform_uv_udp_connect(
    pomelo_platform_uv_t * platform,
    pomelo_address_t * address,
    pomelo_platform_handle_t * udp_handle // Might be NULL
) {
    assert(platform != NULL);
    assert(address != NULL);

    pomelo_platform_uv_udp_mgr_t * manager = platform->udp_mgr;

    // Convert address to sockaddr
    struct sockaddr_storage addr;
    if (pomelo_address_to_sockaddr(address, &addr) < 0) return -1;

    // Acquire socket
    pomelo_platform_uv_udp_t * socket =
        pomelo_pool_acquire(manager->socket_pool, NULL);
    if (!socket) return -1; // Failed to acquire socket

    // Setup socket
    socket->manager = manager;

    // Setup UDP handle
    uv_udp_t * udp = &socket->uv_udp;
    uv_udp_init(manager->uv_loop, udp);
    udp->data = socket;

    int err;

#if UV_UV_CONNECT_AVAILABLE == 1
    // uv_udp_connect is only available after uv version 1.27.0
    err = uv_udp_connect(udp, (struct sockaddr *) &addr);
#else
    struct sockaddr_in bind_addr;
    uv_ip4_addr("0.0.0.0", 0, &bind_addr);
    err = uv_udp_bind(udp, (const struct sockaddr*) &bind_addr, 0);
    if (err == 0) {
        // Copy the target address
        memcpy(&socket->target_addr, &addr, sizeof(struct sockaddr_storage));
    }
#endif

    if (err < 0) {
        pomelo_platform_uv_udp_close(socket);
        return -1;
    }

    // Update the buffer size
    int send_buf_size = POMELO_CLIENT_SOCKET_SNDBUF_SIZE;
    int recv_buf_size = POMELO_CLIENT_SOCKET_RCVBUF_SIZE;

    uv_send_buffer_size((uv_handle_t *) udp, &send_buf_size);
    uv_recv_buffer_size((uv_handle_t *) udp, &recv_buf_size);

    // Start receiving
    if (uv_udp_recv_start(
        udp,
        pomelo_platform_uv_udp_alloc_callback,
        pomelo_platform_uv_udp_recv_callback
    ) < 0) {
        pomelo_platform_uv_udp_close(socket);
        return -1;
    }

    socket->entry = pomelo_list_push_back(manager->sockets, socket);
    if (!socket->entry) {
        pomelo_platform_uv_udp_close(socket);
        return -1;
    }

    // Attach the socket to the handle
    socket->handle = udp_handle;
    if (udp_handle) {
        udp_handle->type = POMELO_PLATFORM_HANDLE_UDP;
        udp_handle->data = socket;
    }

    return 0;
}


void pomelo_platform_uv_udp_stop(
    pomelo_platform_uv_t * platform,
    pomelo_platform_handle_t * udp_handle
) {
    (void) platform;
    assert(udp_handle != NULL);

    // Check the handle
    if (udp_handle->type != POMELO_PLATFORM_HANDLE_UDP) return;

    pomelo_platform_uv_udp_t * socket = udp_handle->data;
    assert(socket != NULL);

    // Stop receiving and close the handle
    pomelo_platform_uv_udp_close(socket);

    // Detach the handle
    udp_handle->type = POMELO_PLATFORM_HANDLE_NONE;
    udp_handle->data = NULL;
    socket->handle = NULL;
}


#ifdef _WIN32
#define BUFFER_LENGTH_TYPE ULONG
#else // Unix
#define BUFFER_LENGTH_TYPE size_t
#endif

int pomelo_platform_uv_udp_send(
    pomelo_platform_uv_t * platform,
    pomelo_platform_handle_t * udp_handle,
    pomelo_address_t * address,
    int niovec,
    pomelo_platform_iovec_t * iovec,
    void * callback_data,
    pomelo_platform_send_cb callback
) {
    assert(platform != NULL);
    assert(udp_handle != NULL);
    assert(iovec != NULL);
    assert(niovec > 0);
    assert(niovec <= POMELO_PLATFORM_UDP_MAX_NUMBER_BUF_VECTORS);

    // Check the handle
    if (udp_handle->type != POMELO_PLATFORM_HANDLE_UDP) return -1;

    pomelo_platform_uv_udp_t * socket = udp_handle->data;
    assert(socket != NULL);

    pomelo_platform_uv_udp_mgr_t * manager = platform->udp_mgr;
    struct sockaddr_storage addr;
    if (address != NULL && pomelo_address_to_sockaddr(address, &addr) < 0) {
        return -1;
    }

    pomelo_platform_uv_send_t * send =
        pomelo_pool_acquire(manager->send_pool, NULL);
    if (!send) return -1; // Failed to acquire send

    send->callback = callback;
    send->callback_data = callback_data;
    send->socket = socket;
    send->uv_req.data = send;

    uv_buf_t bufs[POMELO_PLATFORM_UDP_MAX_NUMBER_BUF_VECTORS];
    bufs[0].base = (char *) iovec[0].data;
    bufs[0].len = (BUFFER_LENGTH_TYPE) iovec[0].length;

    manager->send_bytes += iovec[0].length;

    if (niovec > 1) {
        bufs[1].base = (char *) iovec[1].data;
        bufs[1].len = (BUFFER_LENGTH_TYPE) iovec[1].length;
        manager->send_bytes += iovec[1].length;
    }

    int ret = uv_udp_send(
        &send->uv_req,
        &socket->uv_udp,
        bufs,
        niovec,
        address
            ? ((struct sockaddr *) &addr)
#if UV_UV_CONNECT_AVAILABLE == 1
            : NULL,
#else
            : (const struct sockaddr *) &socket->target_addr,
#endif
        pomelo_platform_uv_send_done
    );

    if (ret == 0) {
        manager->sending_requests++;
    }
    return ret;
}


void pomelo_platform_uv_udp_recv_start(
    pomelo_platform_uv_t * platform,
    pomelo_platform_handle_t * udp_handle,
    void * context,
    pomelo_platform_alloc_cb alloc_callback,
    pomelo_platform_recv_cb recv_callback
) {
    (void) platform;
    assert(udp_handle != NULL);

    // Check the handle
    if (udp_handle->type != POMELO_PLATFORM_HANDLE_UDP) return;

    pomelo_platform_uv_udp_t * socket = udp_handle->data;
    assert(socket != NULL);

    socket->alloc_callback = alloc_callback;
    socket->recv_callback = recv_callback;
    socket->context = context;
}


/* -------------------------------------------------------------------------- */
/*                               Private APIs                                 */
/* -------------------------------------------------------------------------- */

void pomelo_platform_uv_udp_alloc_callback(
    uv_handle_t * handle,
    size_t suggested_size,
    uv_buf_t * buf
) {
    (void) suggested_size;

    pomelo_platform_uv_udp_t * socket = handle->data;
    assert(socket != NULL);
    assert(socket->alloc_callback != NULL);

    pomelo_platform_iovec_t iovec;
    memset(&iovec, 0, sizeof(pomelo_platform_iovec_t));

    if (!socket->alloc_callback) {
        // No alloc callback is set
        buf->base = NULL;
        buf->len = 0;
        return;
    }

    socket->alloc_callback(socket->context, &iovec);

    buf->base = (char *) iovec.data;
    buf->len = (BUFFER_LENGTH_TYPE) iovec.length;
}


void pomelo_platform_uv_udp_recv_callback(
    uv_udp_t * handle,
    ssize_t nread,
    const uv_buf_t * buf,
    const struct sockaddr * addr,
    unsigned flags
) {
    pomelo_platform_uv_udp_t * socket = handle->data;
    assert(socket != NULL);
    (void) flags;

    pomelo_address_t address;
    int address_valid = addr
        ? pomelo_address_from_sockaddr(&address, addr) == 0
        : 0;

    pomelo_platform_iovec_t iovec;
    iovec.data = (uint8_t *) buf->base;
    iovec.length = (nread > 0) ? (size_t) nread : 0;

    if (nread > 0) {
        socket->manager->recv_bytes += nread;
    }

    if (socket->recv_callback) {
        socket->recv_callback(
            socket->context,
            address_valid ? &address : NULL,
            &iovec,
            (address_valid && nread > 0) ? 0 : -1
        );
    }
}


void pomelo_platform_uv_send_done(uv_udp_send_t * req, int status) {
    pomelo_platform_uv_send_t * send = req->data;
    pomelo_platform_uv_udp_t * socket = send->socket;
    pomelo_platform_uv_udp_mgr_t * manager = socket->manager;

    // Capture the values
    void * callback_data = send->callback_data;
    pomelo_platform_send_cb callback = send->callback;

    // Release the sending pass
    pomelo_pool_release(manager->send_pool, send);

    // Finally, call the callback
    if (callback) {
        callback(callback_data, status);
    }

    manager->sending_requests--;
    pomelo_platform_uv_udp_mgr_check_closed(manager);
}


void pomelo_platform_uv_udp_mgr_check_closed(
    pomelo_platform_uv_udp_mgr_t * manager
) {
    assert(manager != NULL);
    if (manager->sockets->size == 0 && manager->sending_requests == 0) {
        pomelo_platform_uv_udp_mgr_on_closed(manager);
    }
}


void pomelo_platform_uv_udp_on_closed(uv_udp_t * udp) {
    pomelo_platform_uv_udp_t * socket = udp->data;
    pomelo_platform_uv_udp_mgr_t * manager = socket->manager;

    pomelo_list_remove(manager->sockets, socket->entry);
    pomelo_pool_release(manager->socket_pool, socket);

    pomelo_platform_uv_udp_mgr_check_closed(manager);
}


void pomelo_platform_uv_udp_close(pomelo_platform_uv_udp_t * socket) {
    assert(socket != NULL);
    if (socket->closing) return; // Already closing

    // Detach the handle
    pomelo_platform_handle_t * handle = socket->handle;
    socket->handle = NULL;
    if (handle) {
        handle->type = POMELO_PLATFORM_HANDLE_NONE;
        handle->data = NULL;
    }

    socket->closing = true;
    uv_close(
        (uv_handle_t *) &socket->uv_udp,
        (uv_close_cb) pomelo_platform_uv_udp_on_closed
    );
}
