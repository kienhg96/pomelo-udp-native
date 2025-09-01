#include "platform-uv.h"


// Glue code for platform uv


void pomelo_platform_set_extra(pomelo_platform_t * platform, void * data) {
    pomelo_platform_uv_set_extra((pomelo_platform_uv_t *) platform, data);
}


void * pomelo_platform_get_extra(pomelo_platform_t * platform) {
    return pomelo_platform_uv_get_extra((pomelo_platform_uv_t *) platform);
}


int pomelo_platform_close(
    pomelo_platform_t * platform,
    pomelo_platform_callback callback
) {
    return pomelo_platform_uv_close(
        (pomelo_platform_uv_t *) platform,
        callback
    );
}


int pomelo_platform_acquire_executor(
    pomelo_platform_t * platform,
    pomelo_platform_handle_t * executor_handle
) {
    return pomelo_platform_uv_acquire_executor(
        (pomelo_platform_uv_t *) platform,
        executor_handle
    );
}


void pomelo_platform_release_executor(
    pomelo_platform_t * platform,
    pomelo_platform_handle_t * executor_handle
) {
    pomelo_platform_uv_release_executor(
        (pomelo_platform_uv_t *) platform,
        executor_handle
    );
}


int pomelo_platform_executor_submit(
    pomelo_platform_t * platform,
    pomelo_platform_handle_t * executor_handle,
    pomelo_platform_task_entry entry,
    void * data
) {
    return pomelo_platform_uv_executor_submit(
        (pomelo_platform_uv_t *) platform,
        executor_handle,
        entry,
        data
    );
}


uint64_t pomelo_platform_hrtime(pomelo_platform_t * platform) {
    return pomelo_platform_uv_hrtime((pomelo_platform_uv_t *) platform);
}


uint64_t pomelo_platform_now(pomelo_platform_t * platform) {
    return pomelo_platform_uv_now((pomelo_platform_uv_t *) platform);
}


int pomelo_platform_timer_start(
    pomelo_platform_t * platform,
    pomelo_platform_timer_entry entry,
    uint64_t timeout_ms,
    uint64_t repeat_ms,
    void * data,
    pomelo_platform_handle_t * timer_handle
) {
    return pomelo_platform_uv_timer_start(
        (pomelo_platform_uv_t *) platform,
        entry,
        timeout_ms,
        repeat_ms,
        data,
        timer_handle
    );
}


void pomelo_platform_timer_stop(
    pomelo_platform_t * platform,
    pomelo_platform_handle_t * timer_handle
) {
    pomelo_platform_uv_timer_stop(
        (pomelo_platform_uv_t *) platform,
        timer_handle
    );
}


int pomelo_platform_udp_bind(
    pomelo_platform_t * platform,
    pomelo_address_t * address,
    pomelo_platform_handle_t * udp_handle
) {
    return pomelo_platform_uv_udp_bind(
        (pomelo_platform_uv_t *) platform,
        address,
        udp_handle
    );
}


int pomelo_platform_udp_connect(
    pomelo_platform_t * platform,
    pomelo_address_t * address,
    pomelo_platform_handle_t * udp_handle
) {
    return pomelo_platform_uv_udp_connect(
        (pomelo_platform_uv_t *) platform,
        address,
        udp_handle
    );
}


void pomelo_platform_udp_stop(
    pomelo_platform_t * platform,
    pomelo_platform_handle_t * udp_handle
) {
    pomelo_platform_uv_udp_stop((pomelo_platform_uv_t *) platform, udp_handle);
}


int pomelo_platform_udp_send(
    pomelo_platform_t * platform,
    pomelo_platform_handle_t * udp_handle,
    pomelo_address_t * address,
    int nbuffers,
    pomelo_platform_iovec_t * buffers,
    void * callback_data,
    pomelo_platform_send_cb send_callback
) {
    return pomelo_platform_uv_udp_send(
        (pomelo_platform_uv_t *) platform,
        udp_handle,
        address,
        nbuffers,
        buffers,
        callback_data,
        send_callback
    );
}


void pomelo_platform_udp_recv_start(
    pomelo_platform_t * platform,
    pomelo_platform_handle_t * udp_handle,
    void * context,
    pomelo_platform_alloc_cb alloc_callback,
    pomelo_platform_recv_cb recv_callback
) {
    pomelo_platform_uv_udp_recv_start(
        (pomelo_platform_uv_t *) platform,
        udp_handle,
        context,
        alloc_callback,
        recv_callback
    );
}


int pomelo_platform_submit_worker_task(
    pomelo_platform_t * platform,
    pomelo_platform_task_entry entry,
    pomelo_platform_task_complete complete,
    void * data,
    pomelo_platform_handle_t * task_handle
) {
    return pomelo_platform_uv_submit_worker_task(
        (pomelo_platform_uv_t *) platform,
        entry,
        complete,
        data,
        task_handle
    );
}


void pomelo_platform_cancel_worker_task(
    pomelo_platform_t * platform,
    pomelo_platform_handle_t * task_handle
) {
    pomelo_platform_uv_cancel_worker_task(
        (pomelo_platform_uv_t *) platform,
        task_handle
    );
}
