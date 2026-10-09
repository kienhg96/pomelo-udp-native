#include "uv.h"
#include "platform-test.h"
#include "pomelo-test.h"
#include "pomelo/address.h"
#include "platform/platform.h"

// Regression test for N1 (REVIEW.md §6): a UDP bind that fails must not abort.
//
// On the error paths of pomelo_platform_uv_udp_bind / _connect the socket is
// closed (uv_close -> pomelo_platform_uv_udp_on_closed) BEFORE it has been
// pushed onto manager->sockets, so socket->entry is still NULL. The shared
// close sink on_closed used to call pomelo_list_remove(sockets, NULL)
// unconditionally, which trips assert(entry != NULL) (src/utils/list.c) ->
// SIGABRT (or a NULL deref in a no-assert build).
//
// Binding to an address that is not assigned to any local interface
// (203.0.113.1, TEST-NET-3, RFC 5737) makes uv_udp_bind fail synchronously
// with EADDRNOTAVAIL, which exercises exactly that orphan-close path. The fix
// lives in on_closed (the sink shared by all three failing call sites), so this
// single case is a faithful regression guard for the whole family.

static pomelo_platform_t * platform = NULL;
static bool shutdown_done = false;

static void shutdown_callback(pomelo_platform_t * p) {
    (void) p;
    shutdown_done = true;
}

static int pomelo_test_platform_bind_fail(void) {
    pomelo_allocator_t * allocator = pomelo_allocator_default();
    uint64_t alloc_bytes = pomelo_allocator_allocated_bytes(allocator);

    platform = pomelo_test_platform_create(allocator);
    pomelo_check(platform != NULL);

    // An address that cannot be bound on this host -> uv_udp_bind fails and the
    // orphan socket is closed before it is tracked.
    pomelo_address_t addr;
    pomelo_check(pomelo_address_from_string(&addr, "203.0.113.1:19999") == 0);

    pomelo_platform_handle_t handle;
    int ret = pomelo_platform_udp_bind(platform, &addr, &handle);
    // The bind must fail (precondition of this test). Without the fix the loop
    // run below aborts in on_closed; with the fix it drains cleanly.
    pomelo_check(ret != 0);

    // Drain the pending orphan close, then tear the platform down.
    pomelo_check(pomelo_platform_close(platform, shutdown_callback) == 0);
    pomelo_test_platform_run(platform);

    pomelo_check(uv_loop_close(uv_default_loop()) == 0);
    pomelo_test_platform_destroy(platform);

    pomelo_check(shutdown_done == true);
    // The orphan socket must have been released back to its pool: no leak.
    pomelo_check(pomelo_allocator_allocated_bytes(allocator) == alloc_bytes);
    return 0;
}

int main(void) {
    pomelo_run_test(pomelo_test_platform_bind_fail);
    return 0;
}
