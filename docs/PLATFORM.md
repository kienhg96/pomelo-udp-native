# Pomelo Platform Layer (Agent Reference)

> libuv abstraction for async I/O (UDP, timer, worker threadpool, cross-thread executor, time) plus the platform↔protocol adapter bridge. Orientation + build/test: [`/CLAUDE.md`](../CLAUDE.md). Cross-cutting threading/shutdown model: [`docs/ARCHITECTURE.md`](ARCHITECTURE.md) §2–§3.

## Role

The platform layer is the only part of the engine that talks to libuv. It wraps
UDP sockets, timers, the libuv worker threadpool (used solely to offload crypto),
a cross-thread executor (`uv_async`-based task submission onto the loop thread), and
time queries. The protocol layer never calls libuv directly — it goes through the
thin interface in `src/platform/platform.h` / `include/pomelo/platform.h`, and incoming
datagrams come back up through the **adapter** (`src/adapter/default.c`), which supplies
pooled receive buffers and converts raw iovecs into `pomelo_buffer_view_t`. One libuv
loop thread per platform owns nearly all of this state (see ARCHITECTURE §3).

## File map

| File | Responsibility |
|------|----------------|
| `src/platform/platform.h` | Internal platform interface: UDP/timer/worker typedefs, `pomelo_platform_iovec_t`, `pomelo_platform_handle_type`, recv/send/alloc callback signatures |
| `include/pomelo/platform.h` | Public opaque types + common APIs (`close`, `hrtime`/`now`, acquire/release/submit executor) |
| `include/pomelo/platforms/platform-uv.h` | libuv backend public API: options, `pomelo_platform_uv_create`/`_destroy`, `_get_uv_loop`, `_statistic` |
| `src/platform/uv/platform-uv.c` | Platform object lifecycle; orchestrates async shutdown via idle handle + `closed_components` bitmask |
| `src/platform/uv/platform-uv.h` | `pomelo_platform_uv_t` struct, the four managers, `COMPONENT_*` bits, `FLAG_CLOSING` |
| `src/platform/uv/udp.c` | UDP manager: bind/connect/stop/send/recv_start; libuv alloc/recv/send-done callbacks |
| `src/platform/uv/udp.h` | UDP structs/pools, `POMELO_PLATFORM_UDP_MAX_NUMBER_BUF_VECTORS` (2), sndbuf/rcvbuf sizes |
| `src/platform/uv/timer.c` | Timer manager: start/stop, one-shot vs repeat, deferred release on close |
| `src/platform/uv/timer.h` | Timer structs/pool |
| `src/platform/uv/worker.c` | Worker threadpool: `uv_queue_work` submit/cancel; entry runs on worker thread, complete on loop thread |
| `src/platform/uv/worker.h` | Worker manager + task structs |
| `src/platform/uv/executor.c` | Cross-thread executor: double-buffered task lists, `uv_async`, submit/shutdown serialization |
| `src/platform/uv/executor.h` | Executor manager + task structs, `EXECUTOR_FLAG_BUSY`/`_SHUTDOWN` |
| `src/platform/uv/time.c` | `hrtime` (uv_hrtime) and `now` (uv_clock_gettime, ms) |
| `src/platform/uv/glue.c` | Maps public `pomelo_platform_*` symbols to the `pomelo_platform_uv_*` backend |
| `src/adapter/default.c` | Platform↔protocol bridge: recv buffer acquire, iovec↔view, send-buffer ref/unref |

## Key types & entry points

**Handles.** `pomelo_platform_handle_t` (`include/pomelo/platform.h:40`) is a `{ int type; void * data; }`
that the caller owns and the backend fills in. `type` is one of `pomelo_platform_handle_type`
(`src/platform/platform.h:53`): `NONE`, `EXECUTOR`, `TIMER`, `UDP`, `TASK_WORKER`, `TASK_EXECUTOR`.
Each `*_start`/`bind`/`connect`/`acquire` **attaches** the backing object to the handle; each
`stop`/`close`/completion **detaches** it (sets `type = NONE`, `data = NULL`). Every backend entry
re-checks `handle->type` before dereferencing — a stale handle is a no-op, not a crash.

**Manager pattern.** The platform (`pomelo_platform_uv_t`, `platform-uv.h:47`) holds four managers —
`udp_mgr`, `timer_mgr`, `worker_mgr`, `executor_mgr` — created in `pomelo_platform_uv_create`
(`platform-uv.c:55-86`). Each manager owns object pools + a live list and exposes
`_create`/`_destroy`/`_close`/`_on_closed`/`_statistic`.

**UDP** (`src/platform/uv/udp.c`, callbacks run on **loop thread**):
- `pomelo_platform_uv_udp_bind` — `udp.c:123` (server: `uv_udp_bind` + `recv_start`; bound **without** `UV_UDP_REUSEADDR`, so an address already in use fails with `EADDRINUSE` — UDP has no `TIME_WAIT`, a stopped port is reusable immediately; see REVIEW.md §6 N3)
- `pomelo_platform_uv_udp_connect` — `udp.c:189` (client; falls back to bind+stored `target_addr` when `uv_udp_connect` unavailable < libuv 1.27)
- `pomelo_platform_uv_udp_stop` — `udp.c:270`
- `pomelo_platform_uv_udp_send` — `udp.c:299` (asserts `0 < niovec <= MAX_NUMBER_BUF_VECTORS` at `:311-312`; fills at most 2 `uv_buf_t`)
- `pomelo_platform_uv_udp_recv_start` — `udp.c:369` (stores context + alloc/recv callbacks)
- `alloc_callback` — `udp.c:395`; `recv_callback` — `udp.c:423`; `send_done` — `udp.c:458`; `udp_close` — `udp.c:501`

**Timer** (callbacks run on **loop thread**):
- `pomelo_platform_uv_timer_start` — `timer.c:104` (`timeout_ms`, `repeat_ms`; `repeat_ms == 0` → one-shot)
- `pomelo_platform_uv_timer_stop` — `timer.c:161` → `_stop_ex` `timer.c:204`
- `pomelo_platform_uv_timer_callback` — `timer.c:227` (one-shot timer self-stops at `:234-237` *before* firing `entry`)

**Worker** (crypto offload):
- `pomelo_platform_uv_submit_worker_task` — `worker.c:97` (`uv_queue_work`)
- `pomelo_platform_uv_cancel_worker_task` — `worker.c:148`
- `worker_entry` — `worker.c:184` runs on a **worker thread**; `worker_done` — `worker.c:190` runs `complete` on the **loop thread**

**Executor** (cross-thread submit onto loop):
- `pomelo_platform_uv_acquire_executor` — `executor.c:133` (**loop thread only / non-threadsafe**; `uv_async_init`)
- `pomelo_platform_uv_release_executor` — `executor.c:175` (submits a self-release task through the executor)
- `pomelo_platform_uv_executor_submit` — `executor.c:206` (**threadsafe**, any thread; `running` check + enqueue + `uv_async_send` under one mutex, `:237-254`)
- `async_callback` — `executor.c:291` drains tasks on the **loop thread**; `shutdown` — `executor.c:424`

**Time** (**threadsafe**): `pomelo_platform_uv_hrtime` — `time.c:6`; `pomelo_platform_uv_now` — `time.c:12`.

**Lifecycle**: `pomelo_platform_uv_create` — `platform-uv.c:27`; `pomelo_platform_uv_close` — `platform-uv.c:168`;
`pomelo_platform_uv_destroy` — `platform-uv.c:92`. Public wrappers for all of the above live in `glue.c`.

## Threading & lifecycle

Full model in ARCHITECTURE §3; the essentials for this layer:

- **Loop thread owns almost everything.** Every UDP/timer callback, the worker `complete`
  callback, and the executor `async_callback` all run on the single libuv loop thread, serialized
  through the base sequencer. Treat apparent races on manager/socket/timer state as false positives.
- **Worker offload.** `submit_worker_task` → `entry` runs on a libuv **worker thread** (the only
  CPU work off the loop thread, used for crypto); `complete` returns to the loop thread. Buffers
  handed to a worker are atomic-ref-counted and untouched by the loop thread until completion.
- **Cross-thread submit.** `executor_submit` is the only API callable from foreign threads. It
  pushes onto the executor's front task list and signals the loop with `uv_async_send`. The loop's
  `async_callback` swaps front/back lists under the mutex, then drains and runs tasks.
- **Async shutdown.** `pomelo_platform_uv_close` (`platform-uv.c:168`) sets `FLAG_CLOSING`, zeroes
  `closed_components`, and starts the idle handle. `idle_close_cb` (`platform-uv.c:153`) closes the
  idle handle itself (`uv_close` → `idle_close_complete`, `:144`) **and** each manager’s `_close`.
  Each manager `uv_close`es its handles; as each drains it ORs its bit into `closed_components`
  (`COMPONENT_TIMER|UDP|WORKER|THREADSAFE|IDLE`, `platform-uv.h:13-24`). When all five bits are set,
  `pomelo_platform_uv_check_closed` (`platform-uv.c:234`) clears `FLAG_CLOSING` and fires the user
  callback. **Never free a handle's backing object before its `uv_close` callback has run** — that is
  why timer/socket/executor release is deferred into the close callbacks, not done inline.

## Invariants

- **One UDP buf-vector bound.** `udp_send` fills at most `POMELO_PLATFORM_UDP_MAX_NUMBER_BUF_VECTORS`
  (2) `uv_buf_t`; the asserts at `udp.c:311-312` enforce `0 < niovec <= 2` so `uv_udp_send` never reads
  uninitialized `bufs[]`. Callers must not pass more.
- **recv status is derived, not trusted.** `recv_callback` passes `status == 0` only when the address
  parsed *and* `nread > 0` (`udp.c:452`); on failure `iovec.length` is forced to 0 (`udp.c:441`), so a
  negative `nread` never leaks a huge length downstream.
- **Handle attach/detach is symmetric.** A handle is attached on start and detached on close/stop/
  completion; backend entries verify `handle->type` first, so a released handle can't be reused to reach
  a freed object.
- **Executor submit/shutdown are serialized.** `submit` checks `executor->running` and sends under the
  same recursive mutex that `shutdown` uses to CAS `running→false` and `uv_close` the async handle
  (`executor.c:237-254` vs `:440-454`). A concurrent submit therefore either sends before close or
  observes `running==false` and never touches the handle.
- **Shutdown bitmask is all-or-nothing.** The user close callback fires exactly once, only when
  `closed_components == COMPONENT_ALL` (all five bits, including `IDLE`).
- **Time APIs are stateless/threadsafe.** `hrtime`/`now` ignore the platform pointer and call libuv directly.

## Gotchas

- **Alloc callback may yield NULL (pool exhaustion / OOM).** When `pomelo_adapter_alloc_callback`
  (`default.c:186`) can't acquire a buffer it leaves `iovec->data == NULL`; libuv then calls
  `recv_callback` with `base == NULL, nread < 0`. The `FLAG_STARTED` check does **not** short-circuit
  this, so `pomelo_adapter_recv_callback` guards it explicitly: `if (!iovec->data) return;`
  (`default.c:219`) — before the `pomelo_buffer_from_data` pointer-math macro (`((pomelo_buffer_t*)data) - 1`).
  Removing that guard reintroduces a recv-path crash on buffer exhaustion.
- **One-shot timer stops before firing.** `timer_callback` calls `_stop_ex` for non-repeat timers
  *before* invoking `entry` (`timer.c:234-239`); the real pool release is deferred to the `uv_close`
  completion `uv_timer_stop_complete` (`timer.c:192`), never inline in the callback.
- **`destroy` does not force `close`.** `pomelo_platform_uv_destroy` (`platform-uv.c:92`) frees the
  managers' pools/lists directly. If libuv handles are still open on the loop, that is a UAF — always
  run `close` to completion (callback fired) before `destroy`. (Tracked as a follow-up to add an assert.)
- **Executor `release` ordering.** `release_executor` detaches the handle and then submits its own
  release task through the executor (`executor.c:175-203`); the owner must stop submitting before
  release. Shutdown that races a drain sets `FLAG_SHUTDOWN` and defers (`executor.c:332-334, 431-435`).
- **Idle close handle is closed too.** The shutdown idle handle has its own `COMPONENT_IDLE` bit and is
  `uv_close`d in `idle_close_cb` (`platform-uv.c:159`); without it the caller's loop would keep a
  reference to freed platform memory.

## Data flow

**Receive** (`libuv → adapter → protocol`):
1. Datagram arrives; libuv calls `udp_alloc_callback` (`udp.c:395`) → `pomelo_adapter_alloc_callback`
   (`default.c:186`), which acquires a pooled `pomelo_buffer_t` and exposes `buffer->data`/`capacity`
   as the iovec (or leaves it NULL on exhaustion).
2. libuv calls `udp_recv_callback` (`udp.c:423`) → `pomelo_adapter_recv_callback` (`default.c:204`).
   After the NULL-data guard, it reconstructs the buffer via `pomelo_buffer_from_data`, wraps it in a
   `pomelo_buffer_view_t`, and calls `pomelo_adapter_on_recv` into the protocol layer; then `unref`s
   the buffer (balancing the acquire).
3. Protocol reads the header, applies state + replay checks, and decrypts the body on a worker thread.

**Send** (`protocol → udp_send → adapter unref`):
1. Protocol/delivery produce an encrypted PAYLOAD and call `pomelo_adapter_send` (`default.c:140`),
   which `ref`s the buffer and builds a single iovec from `view->buffer->data + offset`.
2. `pomelo_platform_udp_send` → `pomelo_platform_uv_udp_send` (`udp.c:299`) issues `uv_udp_send`.
3. On completion `udp_send_done` (`udp.c:458`) releases the send object and invokes the send callback
   → `process_send_complete` (`default.c:131`) `unref`s the buffer, balancing the send-time `ref`.
