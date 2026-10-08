# Platform Layer Hardening Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Fix the Platform layer findings from `REVIEW.md` §2 — a recv-path NULL crash, a leaked `idle_close` handle, an executor submit/shutdown race, and two low-risk hardening nits.

**Architecture:** Pure C on libuv. Fixes span `src/adapter/default.c`, `src/platform/uv/platform-uv.{c,h}`, `src/platform/uv/executor.c`, `src/platform/uv/udp.c`. The existing integration test `pomelo-test-platform-uv` exercises timer/executor/UDP/worker through a real `uv_loop`.

**Tech Stack:** C11, libuv, CMake + CTest. Harness `test/pomelo-test.h` (`pomelo_check(cond)` aborts on failure).

---

## Ground rules for the implementer (READ FIRST)

1. **Use the `Write` tool (full-file rewrite), NOT `Edit`.** A linter/hook in this repo reverts partial edits. Read the whole file, apply the change, write it back in full.
2. **Do NOT `git push`.** Local commits only.
3. Every commit message MUST end with exactly:
   ```
   Co-Authored-By: claude-flow <ruv@ruv.net>
   ```
4. Branch off `main` first:
   ```bash
   git checkout main
   git checkout -b fix/platform-hardening
   ```
   (If `main` has uncommitted review docs, that's fine — leave them; just branch.)
5. Build + test commands (build dir already configured at `build/`):
   ```bash
   cmake --build build --target pomelo-test-platform-uv -j
   ctest --test-dir build -R pomelo-test-platform-uv --output-on-failure
   ```
   A full-suite sanity run at the end: `ctest --test-dir build --output-on-failure`.
6. One commit per task. Run the platform test after every task.
7. **TDD where practical.** Tasks 2 has a clean failing test. Tasks 1, 3, 4 are defensive/concurrency fixes that cannot be reproduced deterministically in this harness — for those, apply the exact code given, then verify the full platform test still passes (no regression) and re-read the diff. This exception is deliberate; do not invent flaky tests.

---

## Task 1: Guard NULL recv buffer in the adapter (🟠 High — crash/DoS)

**Problem:** `pomelo_buffer_from_data(data)` is the macro `((pomelo_buffer_t *)(data)) - 1`. When the alloc callback cannot provide a buffer (pool exhausted / OOM), `iovec->data` is NULL; libuv then calls the recv callback with `nread < 0` and `buf.base == NULL`; `pomelo_adapter_recv_callback` (STARTED) computes `from_data(NULL)` → a wild pointer and `pomelo_buffer_unref()`s it → crash.

**Files:**
- Modify: `src/adapter/default.c` (`pomelo_adapter_recv_callback`, currently lines 204-230)

- [ ] **Step 1: Read `src/adapter/default.c` fully.** Confirm `pomelo_adapter_recv_callback` matches the snippet below.

Current body:
```c
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
        ...
    }

    pomelo_buffer_unref(buffer);
}
```

- [ ] **Step 2: Apply the guard**

Add a NULL-data check immediately after the STARTED check, before `pomelo_buffer_from_data`. The full function becomes:

```c
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

    // The alloc callback may have failed to provide a buffer (pool exhausted /
    // OOM). In that case libuv still invokes this callback with a NULL base and
    // a negative nread. There is nothing to reconstruct or release.
    if (!iovec->data) {
        return;
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
```

(Rewrite the whole file with `Write`, preserving everything else.)

- [ ] **Step 3: Build & run platform test — verify no regression**

Run:
```bash
cmake --build build --target pomelo-test-platform-uv -j
ctest --test-dir build -R pomelo-test-platform-uv --output-on-failure
```
Expected: PASS (the happy-path recv still works; the guard only short-circuits the NULL case).

- [ ] **Step 4: Commit**

```bash
git add src/adapter/default.c
git commit -m "fix(adapter): guard NULL recv buffer on alloc failure

When the buffer pool is exhausted the alloc callback yields a NULL
buffer; libuv then calls recv with a NULL base. pomelo_buffer_from_data
would turn that into a wild pointer and unref it, crashing. Return early
when iovec->data is NULL.

Co-Authored-By: claude-flow <ruv@ruv.net>"
```

---

## Task 2: Close the `idle_close` handle on platform shutdown (🟡 Medium — leaked handle / UAF)

**Problem:** `platform->idle_close` (a `uv_idle_t`) is `uv_idle_init`'d but only ever `uv_idle_stop`'d, never `uv_close`'d. After the platform is freed, the caller's loop still references that (now-freed) handle, so `uv_loop_close` returns `UV_EBUSY` and a later `uv_run` is UB. Fix by closing the handle as part of shutdown and tracking it as a close component.

**Files:**
- Test: `test/platform-test/platform-uv.c`
- Modify: `src/platform/uv/platform-uv.h`, `src/platform/uv/platform-uv.c`

- [ ] **Step 1: Make the failing test — assert `uv_loop_close` succeeds**

In `test/platform-test/platform-uv.c`, change `pomelo_test_platform_run` from:
```c
void pomelo_test_platform_run(pomelo_platform_t * platform) {
    (void) platform;
    uv_run(uv_default_loop(), UV_RUN_DEFAULT);
    uv_loop_close(uv_default_loop());
}
```
to:
```c
void pomelo_test_platform_run(pomelo_platform_t * platform) {
    (void) platform;
    uv_run(uv_default_loop(), UV_RUN_DEFAULT);
    // All platform handles (including idle_close) must be closed so the loop
    // can be torn down cleanly.
    pomelo_check(uv_loop_close(uv_default_loop()) == 0);
}
```
(`pomelo_check` is already available via `pomelo-test.h`, already included in this file.)

- [ ] **Step 2: Build & run — verify it fails**

Run:
```bash
cmake --build build --target pomelo-test-platform-uv -j
ctest --test-dir build -R pomelo-test-platform-uv --output-on-failure
```
Expected: FAIL — `uv_loop_close` returns `UV_EBUSY` (nonzero) because the stopped-but-unclosed `idle_close` handle is still registered, so `pomelo_check` aborts.

- [ ] **Step 3: Add the IDLE close component in `src/platform/uv/platform-uv.h`**

Find the component block:
```c
#define POMELO_PLATFORM_UV_COMPONENT_TIMER      (1 << 0)
#define POMELO_PLATFORM_UV_COMPONENT_UDP        (1 << 1)
#define POMELO_PLATFORM_UV_COMPONENT_WORKER     (1 << 2)
#define POMELO_PLATFORM_UV_COMPONENT_THREADSAFE (1 << 3)
#define POMELO_PLATFORM_UV_COMPONENT_ALL (    \
    POMELO_PLATFORM_UV_COMPONENT_TIMER      | \
    POMELO_PLATFORM_UV_COMPONENT_UDP        | \
    POMELO_PLATFORM_UV_COMPONENT_WORKER     | \
    POMELO_PLATFORM_UV_COMPONENT_THREADSAFE   \
)
```
Replace with (adds `COMPONENT_IDLE` and includes it in `_ALL`):
```c
#define POMELO_PLATFORM_UV_COMPONENT_TIMER      (1 << 0)
#define POMELO_PLATFORM_UV_COMPONENT_UDP        (1 << 1)
#define POMELO_PLATFORM_UV_COMPONENT_WORKER     (1 << 2)
#define POMELO_PLATFORM_UV_COMPONENT_THREADSAFE (1 << 3)
#define POMELO_PLATFORM_UV_COMPONENT_IDLE       (1 << 4)
#define POMELO_PLATFORM_UV_COMPONENT_ALL (    \
    POMELO_PLATFORM_UV_COMPONENT_TIMER      | \
    POMELO_PLATFORM_UV_COMPONENT_UDP        | \
    POMELO_PLATFORM_UV_COMPONENT_WORKER     | \
    POMELO_PLATFORM_UV_COMPONENT_THREADSAFE | \
    POMELO_PLATFORM_UV_COMPONENT_IDLE         \
)
```

- [ ] **Step 4: Close the idle handle in `src/platform/uv/platform-uv.c`**

Replace the existing `idle_close_cb` (currently lines 144-154):
```c
static void idle_close_cb(uv_idle_t * handle) {
    pomelo_platform_uv_t * platform = handle->data;
    assert(platform != NULL);

    uv_idle_stop(handle);

    pomelo_platform_uv_udp_mgr_close(platform->udp_mgr);
    pomelo_platform_uv_timer_mgr_close(platform->timer_mgr);
    pomelo_platform_uv_executor_mgr_close(platform->executor_mgr);
    pomelo_platform_uv_worker_mgr_close(platform->worker_mgr);
}
```
with (close the idle handle too, and account for it on completion):
```c
static void idle_close_complete(uv_handle_t * handle) {
    pomelo_platform_uv_t * platform = handle->data;
    assert(platform != NULL);

    platform->closed_components |= POMELO_PLATFORM_UV_COMPONENT_IDLE;
    pomelo_platform_uv_check_closed(platform);
}


static void idle_close_cb(uv_idle_t * handle) {
    pomelo_platform_uv_t * platform = handle->data;
    assert(platform != NULL);

    // Stop and close the idle handle itself so the loop can be torn down.
    uv_idle_stop(handle);
    uv_close((uv_handle_t *) handle, idle_close_complete);

    pomelo_platform_uv_udp_mgr_close(platform->udp_mgr);
    pomelo_platform_uv_timer_mgr_close(platform->timer_mgr);
    pomelo_platform_uv_executor_mgr_close(platform->executor_mgr);
    pomelo_platform_uv_worker_mgr_close(platform->worker_mgr);
}
```

Note: `idle_close_complete` only sets its bit and calls `pomelo_platform_uv_check_closed`, which fires the user callback only once **all** components (now including IDLE) are closed — so the user's close callback still fires exactly once, after every handle is closed.

- [ ] **Step 5: Build & run — verify pass**

Run:
```bash
cmake --build build --target pomelo-test-platform-uv -j
ctest --test-dir build -R pomelo-test-platform-uv --output-on-failure
```
Expected: PASS — `uv_loop_close` now returns 0.

- [ ] **Step 6: Commit**

```bash
git add src/platform/uv/platform-uv.h src/platform/uv/platform-uv.c test/platform-test/platform-uv.c
git commit -m "fix(platform): close idle_close handle on shutdown

The idle_close uv_idle_t was stopped but never uv_close'd, leaving a
dangling handle on the caller's loop (uv_loop_close returned EBUSY and a
later uv_run was UB). Close it and track it as a new close component so
the user callback still fires once all handles are closed.

Co-Authored-By: claude-flow <ruv@ruv.net>"
```

---

## Task 3: Serialize executor submit vs shutdown (🟡 Medium — cross-thread race)

**Problem:** `pomelo_platform_uv_executor_submit` (callable from any thread) checks `executor->running` and later calls `uv_async_send`, while `pomelo_platform_executor_shutdown` (loop thread) clears `running` and `uv_close`s the async handle. The check and the send are not serialized against the clear and the close, so submit can `uv_async_send` on a closing handle and enqueue a task onto an executor about to be released. Fix by performing the `running` check + list push + `uv_async_send` under the executor's (recursive) mutex, and the `running` clear + `uv_close` under the same mutex.

**Files:**
- Modify: `src/platform/uv/executor.c` (`pomelo_platform_uv_executor_submit`, `pomelo_platform_executor_shutdown`)

- [ ] **Step 1: Rewrite `pomelo_platform_uv_executor_submit`**

Replace the current body (currently lines 206-262) with:

```c
int pomelo_platform_uv_executor_submit(
    pomelo_platform_uv_t * platform,
    pomelo_platform_handle_t * executor_handle,
    pomelo_platform_task_entry entry,
    void * data
) {
    assert(platform != NULL);
    assert(executor_handle != NULL);
    assert(entry != NULL);

    if (executor_handle->type != POMELO_PLATFORM_HANDLE_EXECUTOR) {
        return -1; // Invalid handle type
    }

    pomelo_platform_executor_t * executor = executor_handle->data;
    pomelo_platform_uv_executor_mgr_t * manager = executor->manager;

    // Acquire new task
    pomelo_platform_uv_task_executor_t * task =
        pomelo_pool_acquire(manager->task_pool, NULL);
    if (!task) {
        return -1; // Failed to acquire new task
    }
    task->manager = manager;
    task->entry = entry;
    task->data = data;

    uv_mutex_t * mutex = &executor->mutex;
    int result = 0;

    /* -------------------------  Critical Begin  ----------------------- */
    uv_mutex_lock(mutex);

    // Check running state and signal the loop under the same lock that
    // shutdown uses, so submit never races a concurrent shutdown.
    if (!pomelo_atomic_int64_load(&executor->running)) {
        result = -1; // Executor stopped; must not touch the async handle
    } else {
        pomelo_list_entry_t * list_entry =
            pomelo_list_push_back(executor->tasks_front, task);
        if (!list_entry) {
            result = -1; // Failed to append to list
        } else if (uv_async_send(&executor->uv_async) < 0) {
            pomelo_list_remove(executor->tasks_front, list_entry);
            result = -1; // Failed to send signal
        }
    }

    uv_mutex_unlock(mutex);
    /* --------------------------  Critical End  ------------------------ */

    if (result != 0) {
        pomelo_platform_uv_task_executor_release(task);
        return -1;
    }

    pomelo_atomic_uint64_fetch_add(&manager->task_counter, 1);
    return 0;
}
```

- [ ] **Step 2: Rewrite `pomelo_platform_executor_shutdown`**

Replace the current body (currently lines 422-472) with (moves the `running` CAS and `uv_close` inside the mutex scope):

```c
int pomelo_platform_executor_shutdown(
    pomelo_platform_executor_t * executor,
    pomelo_platform_uv_executor_mgr_t * manager
) {
    assert(executor != NULL);
    assert(manager != NULL);

    if (executor->flags & POMELO_UV_EXECUTOR_FLAG_BUSY) {
        // Just set the flag for later shutdown
        executor->flags |= POMELO_UV_EXECUTOR_FLAG_SHUTDOWN;
        return 0; // Executor is busy
    }

    uv_mutex_t * mutex = &executor->mutex;

    /* ----------- Begin mutex scope ----------- */
    uv_mutex_lock(mutex);

    if (!pomelo_atomic_int64_compare_exchange(
        &executor->running, /* expected */ true, /* desired */ false
    )) {
        uv_mutex_unlock(mutex);
        assert(false);
        return -1; // Executor is not running
    }

    // Close the async handle under the lock so a concurrent submit either
    // observes running==true and sends before this, or observes running==false
    // and never touches the handle.
    uv_async_t * async = &executor->uv_async;
    uv_close((uv_handle_t *) async, executor_shutdown_complete);

    // Update task counter
    pomelo_atomic_uint64_fetch_sub(
        &manager->task_counter,
        executor->tasks_front->size + executor->tasks_back->size
    );

    pomelo_platform_uv_task_executor_t * task = NULL;
    pomelo_list_t * tasks = executor->tasks_front;
    while (pomelo_list_pop_front(tasks, &task) == 0) {
        pomelo_platform_uv_task_executor_release(task);
    }

    tasks = executor->tasks_back;
    while (pomelo_list_pop_front(tasks, &task) == 0) {
        pomelo_platform_uv_task_executor_release(task);
    }

    uv_mutex_unlock(mutex);
    /* ------------ End mutex scope ------------ */

    return 0;
}
```

The executor's mutex is created with `uv_mutex_init_recursive` (see `pomelo_platform_executor_on_alloc`), so re-entrant locking from the same loop thread (e.g. shutdown invoked from inside the async callback after it has already released the lock) remains safe.

- [ ] **Step 3: Build & run platform test — verify no regression**

Run:
```bash
cmake --build build --target pomelo-test-platform-uv -j
ctest --test-dir build -R pomelo-test-platform-uv --output-on-failure
```
Expected: PASS. `pomelo_test_platform_job` submits tasks to the executor from a worker thread and releases it, exercising the submit/shutdown paths; it must stay green. (The race itself is not deterministically reproducible in-harness; this step verifies the refactor did not break normal operation.)

- [ ] **Step 4: Commit**

```bash
git add src/platform/uv/executor.c
git commit -m "fix(platform): serialize executor submit against shutdown

submit checked running then uv_async_send'd outside any lock, while
shutdown cleared running and uv_close'd the async handle. A cross-thread
submit could signal a closing handle and enqueue onto a dying executor.
Perform the running check + enqueue + async_send and the running clear +
async close under the executor's recursive mutex.

Co-Authored-By: claude-flow <ruv@ruv.net>"
```

---

## Task 4: Low-risk hardening — `udp_send` iovec bound + negative nread length (🔵/⚪)

**Problem:** (a) `pomelo_platform_uv_udp_send` fills at most 2 entries of `bufs[POMELO_PLATFORM_UDP_MAX_NUMBER_BUF_VECTORS]` but passes the caller's `niovec` to `uv_udp_send`; `niovec > 2` would read uninitialized stack. (b) `pomelo_platform_uv_udp_recv_callback` sets `iovec.length = (size_t) nread` even when `nread < 0`, producing a huge length.

**Files:**
- Modify: `src/platform/uv/udp.c`

- [ ] **Step 1: Add the iovec-count assert in `pomelo_platform_uv_udp_send`**

Right after the existing `assert(niovec > 0);` (near line 311), add:
```c
    assert(niovec <= POMELO_PLATFORM_UDP_MAX_NUMBER_BUF_VECTORS);
```

- [ ] **Step 2: Clamp the recv length on error in `pomelo_platform_uv_udp_recv_callback`**

Find:
```c
    pomelo_platform_iovec_t iovec;
    iovec.data = (uint8_t *) buf->base;
    iovec.length = (size_t) nread;
```
Replace with:
```c
    pomelo_platform_iovec_t iovec;
    iovec.data = (uint8_t *) buf->base;
    iovec.length = (nread > 0) ? (size_t) nread : 0;
```

- [ ] **Step 3: Build & run platform test — verify no regression**

Run:
```bash
cmake --build build --target pomelo-test-platform-uv -j
ctest --test-dir build -R pomelo-test-platform-uv --output-on-failure
```
Expected: PASS.

- [ ] **Step 4: Commit**

```bash
git add src/platform/uv/udp.c
git commit -m "harden(platform): bound udp_send iovec count and clamp recv length

Assert niovec stays within the bufs array, and set the recv iovec length
to 0 on non-positive nread instead of a wrapped size_t.

Co-Authored-By: claude-flow <ruv@ruv.net>"
```

---

## Final verification

- [ ] Run the full suite to confirm nothing else regressed:
```bash
ctest --test-dir build --output-on-failure
```
Expected: all tests pass.
- [ ] `git log --oneline` should show 4 new commits on `fix/platform-hardening`. Do NOT push.

---

## Out of scope (follow-up)

- 🔵 `platform-uv.c:92` — `pomelo_platform_uv_destroy` does not enforce that `close()` completed first. Adding a state assert touches the public destroy contract; handle separately.

---

## Self-Review (completed by plan author)

- **Spec coverage:** Task 1 = 🟠 recv NULL crash; Task 2 = 🟡 idle_close leak (TDD); Task 3 = 🟡 executor race; Task 4 = 🔵/⚪ udp nits. The 🔵 destroy-before-close contract is explicitly deferred.
- **Placeholder scan:** none — every step has concrete code/commands.
- **Type consistency:** `POMELO_PLATFORM_UV_COMPONENT_IDLE` added to both the per-bit list and `_ALL`; `idle_close_complete` matches the `uv_close_cb` signature `void(uv_handle_t *)`; executor changes keep existing symbol names (`executor_shutdown_complete`, `pomelo_platform_uv_task_executor_release`).
- **TDD note:** Task 2 is strict TDD (fail→fix→pass). Tasks 1/3/4 are defensive/concurrency fixes verified by no-regression on the integration test — a deliberate, documented exception because the triggering conditions (pool exhaustion, cross-thread race) are not deterministically reproducible in this harness.
