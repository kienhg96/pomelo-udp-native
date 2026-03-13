#include "platform-test.h"
#include "pomelo-test.h"
#include "utils/pool.h"
#include "protocol/socket.h"


#define TEST_TIMER_INTERVAL 100
#define CALLBACK_DATA 1122
#define CLIENT_TO_SERVER_DATA 1233
#define SERVER_TO_CLIENT_DATA 2231


// Common platform entities
static pomelo_pool_t * buffer_pool = NULL;
static pomelo_platform_t * platform = NULL;
static pomelo_platform_handle_t timer;
static pomelo_platform_handle_t executor;

static int test_work_result = 0;
static int test_work_entry_result = 0;
static int test_work_cancel_result = 0;

static int test_timer_counter = 0;
static int test_timer_result = 0;

static int test_client_result = 0;
static int test_server_result = 0;
static int tesk_main_task_result = 0;

static uint64_t test_timer_last_callback_time = 0;
static int platform_done_counter = 0;
static bool platform_done_flag = false;


// socket platform entities
static pomelo_platform_handle_t server;
static pomelo_platform_handle_t client;

// Temp variables
static void * temp_data = NULL;


static void platform_shutdown_callback(pomelo_platform_t * platform) {
    (void) platform;
    pomelo_track_function();
    platform_done_flag = true;
}


static void platform_done(void) {
    platform_done_counter++;

    if (platform_done_counter == 4) {
        // Then shutdown the platform job
        // TODO: Check this test
        pomelo_platform_close(platform, platform_shutdown_callback);
    }
}


static void alloc_callback(
    void * callback_data,
    pomelo_platform_iovec_t * buffer
) {
    (void) callback_data;
    buffer->data = pomelo_pool_acquire(buffer_pool, NULL);
    buffer->length = buffer->data ? POMELO_BUFFER_CAPACITY : 0;
}


static void pomelo_test_work_cancel_entry(void * data) {
    (void) data;
    pomelo_track_function();
}


static void pomelo_test_work_cancel_done(void * data, uint32_t flags) {
    pomelo_track_function();
    (void) data;

    test_work_cancel_result = (flags & POMELO_PLATFORM_TASK_FLAG_CANCELED);
}


static void pomelo_test_work_entry(void * data) {
    pomelo_track_function();

    *((int *) data) = 1;
}


static void pomelo_test_work_done(void * data, uint32_t flags) {
    pomelo_track_function();
    (void) data;

    test_work_result = (flags == 0);

    // Test cancel case
    pomelo_platform_handle_t handle;
    int ret = pomelo_platform_submit_worker_task(
        platform,
        pomelo_test_work_cancel_entry,
        pomelo_test_work_cancel_done,
        NULL,
        &handle
    );

    pomelo_check(ret == 0);
    pomelo_platform_cancel_worker_task(platform, &handle);
    platform_done();
}

static int pomelo_test_platform_work(void) {
    pomelo_track_function();

    // Test the platform work
    int ret = pomelo_platform_submit_worker_task(
        platform,
        pomelo_test_work_entry,
        pomelo_test_work_done,
        &test_work_entry_result,
        NULL
    );

    pomelo_check(ret == 0);
    return 0;
}


static void pomelo_test_timer_callback(void * data) {
    pomelo_track_function();

    int * counter = data;
    (*counter)++;

    uint64_t time = pomelo_platform_hrtime(platform);
    uint64_t delta = time - test_timer_last_callback_time;
    if (delta < TEST_TIMER_INTERVAL) {
        printf("[!] Timer test did NOT pass.\n");
        return;
    }
    test_timer_last_callback_time = time;

    if (*counter == 5) {
        pomelo_platform_timer_stop(platform, &timer);
        test_timer_result = 1;

        platform_done();
    }
}


static int pomelo_test_platform_timer(void) {
    pomelo_track_function();

    // Test the platform timer
    test_timer_last_callback_time = pomelo_platform_hrtime(platform);

    int ret = pomelo_platform_timer_start(
        platform,
        pomelo_test_timer_callback,
        TEST_TIMER_INTERVAL, // Timeout
        TEST_TIMER_INTERVAL, // Inteval
        &test_timer_counter,
        &timer
    );

    pomelo_check(ret == 0);
    return 0;
}


static void stop_sockets(void) {
    pomelo_platform_udp_stop(platform, &server);
    pomelo_platform_udp_stop(platform, &client);

    platform_done();
}


static void server_send_callback(
    void * send_callback_data,
    int status
) {
    (void) status;
    pomelo_track_function();
    // The sending callback

    pomelo_pool_release(buffer_pool, send_callback_data);
}


static void server_recv_callback(
    void * recv_callback_data,
    pomelo_address_t * address,
    pomelo_platform_iovec_t * buffer,
    int status
) {
    (void) recv_callback_data;
    pomelo_track_function();

    // Client has sent message successfully
    test_client_result = 1;

    if (status < 0) {
        // Invalid, discard
        pomelo_pool_release(buffer_pool, buffer->data);
        return;
    }

    // The receiving callback

    pomelo_payload_t payload;
    payload.position = 0;
    payload.capacity = buffer->length;
    payload.data = buffer->data;

    int value = 0;
    int ret = pomelo_payload_read_int32(&payload, &value);
    printf("[i] Received data: %d with result %d\n", value, ret);
    
    // Check the data
    if (value != CLIENT_TO_SERVER_DATA) {
        printf("[!] Received data check failed");
        test_client_result = 0;
        test_server_result = 0;
        pomelo_pool_release(buffer_pool, payload.data);
        stop_sockets();
        return;
    }

    payload.position = 0;
    pomelo_payload_write_int32(&payload, SERVER_TO_CLIENT_DATA); // Write an integer

    // Send the payload from client to server
    printf("[i] Server send payload to Client port: %d\n", address->port);

    pomelo_platform_iovec_t buf;
    buf.data = payload.data;
    buf.length = payload.position;

    ret = pomelo_platform_udp_send(
        platform,
        &server,
        address,
        /* nbufs = */ 1,
        &buf,
        payload.data,
        server_send_callback
    );

    if (ret < 0) {
        printf("[!] Failed to send payload from server to client\n");
        test_client_result = 0;
        test_server_result = 0;
        pomelo_pool_release(buffer_pool, payload.data);
        stop_sockets();
        return;
    }
}



static void client_recv_callback(
    void * recv_callback_data,
    pomelo_address_t * address,
    pomelo_platform_iovec_t * buffer,
    int status
) {
    (void) recv_callback_data;
    (void) address;
    if (status < 0) {
        // Invalid, discard
        pomelo_pool_release(buffer_pool, buffer->data);
        return;
    }

    pomelo_track_function();

    // The receiving callback

    // Free the payload
    pomelo_pool_release(buffer_pool, buffer->data);

    test_server_result = 1;

    stop_sockets();
}


static void client_send_callback(
    void * send_callback_data,
    int status
) {
    (void) status;
    pomelo_track_function();

    pomelo_pool_release(buffer_pool, send_callback_data);
}


static const char * addr_str = "127.0.0.1:8888";


static int pomelo_test_platform_socket(void) {
    pomelo_track_function();

    printf("[w] This test will bind to the address %s\n", addr_str);

    // Create address first
    pomelo_address_t addr;

    pomelo_check(pomelo_address_from_string(&addr, addr_str) == 0);

    // Server starts listening on specific address
    int ret = pomelo_platform_udp_bind(platform, &addr, &server);
    pomelo_check(ret == 0);
    
    // Client starts connecting to server
    ret = pomelo_platform_udp_connect(platform, &addr, &client);
    pomelo_check(ret == 0);

    pomelo_platform_udp_recv_start(
        platform,
        &server,
        NULL,
        alloc_callback,
        server_recv_callback
    );

    pomelo_platform_udp_recv_start(
        platform,
        &client,
        NULL,
        alloc_callback,
        client_recv_callback
    );

    // Acquire one payload from pool
    uint8_t * data = pomelo_pool_acquire(buffer_pool, NULL);
    pomelo_check(data != NULL);

    pomelo_payload_t payload;
    payload.position = 0;
    payload.capacity = POMELO_BUFFER_CAPACITY;
    payload.data = data;

    pomelo_payload_write_int32(&payload, CLIENT_TO_SERVER_DATA); // Write an integer

    // Send the payload from client to server
    printf("[i] Client send payload to Server\n");
    
    pomelo_platform_iovec_t buf;
    buf.data = payload.data;
    buf.length = payload.position;

    ret = pomelo_platform_udp_send(
        platform,
        &client,
        /* address = */ NULL,
        /* nbufs = */ 1,
        &buf,
        payload.data,
        client_send_callback
    );

    pomelo_check(ret == 0);
    return 0;
}


static void pomelo_test_platform_job_callback(void * data) {
    pomelo_track_function();
    tesk_main_task_result = (data == temp_data);

    // Release the threadsafe executor
    pomelo_platform_release_executor(platform, &executor);
    platform_done();
}


static int pomelo_test_platform_job(void) {
    int ret = pomelo_platform_executor_submit(
        platform,
        &executor,
        pomelo_test_platform_job_callback,
        temp_data
    );
    pomelo_check(ret == 0);
    return 0;
}


int pomelo_test_platform(void) {
    pomelo_allocator_t * allocator = pomelo_allocator_default();
    uint64_t alloc_bytes = pomelo_allocator_allocated_bytes(allocator);

    pomelo_pool_root_options_t pool_options = {
        .allocator = allocator,
        .element_size = POMELO_BUFFER_CAPACITY
    };
    buffer_pool = pomelo_pool_root_create(&pool_options);
    pomelo_check(buffer_pool != NULL);

    // Create platform first
    platform = pomelo_test_platform_create(allocator);
    pomelo_check(platform != NULL);

    // Acquire the threadsafe executor
    int ret = pomelo_platform_acquire_executor(platform, &executor);
    pomelo_check(ret == 0);

    // Try to get the current time
    uint64_t current = pomelo_platform_hrtime(platform);
    pomelo_check(current > 0);

    // 4 tests
    pomelo_check(pomelo_test_platform_work() == 0);
    pomelo_check(pomelo_test_platform_timer() == 0);
    pomelo_check(pomelo_test_platform_socket() == 0);
    pomelo_check(pomelo_test_platform_job() == 0);

    pomelo_test_platform_run(platform);

    pomelo_pool_destroy(buffer_pool);
    pomelo_test_platform_destroy(platform);

    pomelo_check(test_work_result == 1);
    pomelo_check(test_work_entry_result == 1);
    pomelo_check(test_work_cancel_result == 1);
    pomelo_check(test_timer_result == 1);
    pomelo_check(test_timer_result == 1);
    pomelo_check(test_client_result == 1);
    pomelo_check(test_server_result == 1);
    pomelo_check(tesk_main_task_result == 1);
    pomelo_check(platform_done_flag == true);

    // Check memleak
    pomelo_check(pomelo_allocator_allocated_bytes(allocator) == alloc_bytes);

    return 0;
}


int main(void) {
    pomelo_run_test(pomelo_test_platform);
    return 0;
}
