# Pomelo UDP Native — Architecture (Agent Reference)

Cross-cutting reference for agents. For orientation + build/test, start at [`/CLAUDE.md`](../CLAUDE.md).
Per-layer detail lives in the sibling docs (`CRYPTO.md`, `PLATFORM.md`, `PROTOCOL.md`, `DELIVERY.md`, `API.md`).

---

## 1. Layering & thin interfaces

Five layers, built bottom-up. They depend on each other only through **thin interface
links** — function-pointer tables (`*_methods_t`) and opaque handles — not tight coupling.
A change inside one layer does not ripple into the others, and each layer can be reviewed,
tested, and patched independently.

```
          ┌─────────────────────────────────────────────┐
  public  │  API      Socket · Session · Channel · Message · Plugin
          │           src/api/ , include/pomelo/api.h
          ├─────────────────────────────────────────────┤
          │  Delivery Reliable/Sequenced/Unreliable · parcel↔fragment · ACK
          │           src/delivery/
          ├─────────────────────────────────────────────┤
          │  Protocol Handshake · peer state · per-packet AEAD · replay
          │           src/protocol/
          ├───────────────┬─────────────────────────────┤
          │  Crypto       │  Platform (libuv)
          │  src/crypto/  │  src/platform/uv/
          └───────────────┴─────────────────────────────┘
  support: Adapter (platform↔protocol glue) · Base (ref/buffer/payload/pipeline/sequencer)
           · Utils (list/array/heap/map/pool/atomic/mutex)
```

- **API ↔ Delivery / Protocol**: a `pomelo_session_t` dispatches through `channel->methods->send` (`src/api/channel.h`); builtin sessions wrap the delivery/protocol layers, plugin sessions wrap custom transports.
- **Protocol ↔ Platform**: via the **Adapter** (`src/adapter/`). The protocol socket drives `pomelo_platform_udp_*`; incoming datagrams flow back through the adapter's alloc/recv callbacks.
- **Protocol/Delivery ↔ Crypto**: direct calls into `src/crypto/` for token and per-packet encryption; the heavy work runs on a worker thread (see §3).

---

## 2. Memory model

Three cooperating mechanisms; no manual `malloc`/`free` scattered through the layers.

### Atomic lock-free ref-counting — `src/base/ref.c`
`pomelo_reference_t` (`src/base/ref.h`) embeds an atomic counter and a finalize callback.
`pomelo_reference_ref` uses a CAS loop (`ref.c:16`) and **refuses to revive a finalized
object** (returns false when the count already hit 0); `pomelo_reference_unref` runs the
finalize callback when the count reaches 0. Ref-counting is atomic because some objects
cross the worker-thread boundary (see §3).

Ref-counted objects and their `*_ref`/`*_unref` wrappers:
- `pomelo_message_t` — `src/api/message.c:31,39`
- `pomelo_buffer_t` — `src/base/buffer.c:265,271`
- `pomelo_delivery_parcel_t` — `src/delivery/parcel.c:193,199`
- `pomelo_protocol_crypto_context_t` — `src/protocol/crypto.c:44,52`

**Rule:** every `*_ref` is balanced by a `*_unref`. A send path typically refs the object
on submit and unrefs it on completion (e.g. `pomelo_message_prepare_send` / `finish_send`,
`src/api/message.c:179,189`).

### Object pools — `src/utils/pool.c`
Peers, packets, parcels, fragments, receivers, senders, timers, tasks, messages, etc. are
recycled through pools (`pomelo_pool_acquire` `pool.c:88`, `pomelo_pool_release`). Pools can
be `zero_init` and `synchronized` (mutex-guarded) when acquired from multiple threads
(e.g. the executor task/executor pools, `src/platform/uv/executor.c`). A pool's `on_alloc`
/`on_free` callbacks run one-time construction/destruction of the pooled object's owned
resources (lists, sub-objects).

### Allocator abstraction — `src/base/allocator.c`
Every subsystem receives a `pomelo_allocator_t *` and allocates through it, so allocation
can be instrumented or replaced. `pomelo_allocator_allocated_bytes` backs the leak checks in
tests (e.g. `test/platform-test/platform-test.c`).

---

## 3. Threading & concurrency model

**One libuv loop thread per platform owns nearly all state.** Understanding this is the key
to reading the codebase correctly.

### Sequencer — `src/base/sequencer.c`
Within the loop thread, work is serialized through a `pomelo_sequencer_t`: when a task is
submitted while another is running, it is queued and run only after the current one fully
returns (`src/base/sequencer.h`). The `pomelo_pipeline_t` (`src/base/pipeline.c`) composes a
fixed series of steps over a sequencer. **Consequence:** two sequencer tasks never overlap,
so state they touch is effectively single-threaded. Most apparent races (e.g. on
`peer->state`, sessions, buses) are **not** races.

### Worker threadpool — `src/platform/uv/worker.c`
The only CPU work pushed off the loop thread is **crypto**. `pomelo_platform_uv_submit_worker_task`
(`worker.c:97`) queues `uv_queue_work`; the `entry` runs on a libuv worker thread
(`worker.c:184`), the `complete` callback runs back on the **loop thread** (`worker.c:190`).
Callers: `src/protocol/receiver.c`, `src/protocol/sender.c`, `src/delivery/receiver.c`,
`src/delivery/sender.c`. The buffer handed to a worker is ref-counted and not touched by the
loop thread until the worker completes — hence atomic ref-counting (§2).

### Cross-thread executor — `src/platform/uv/executor.c`
Other threads submit tasks to the loop thread via `pomelo_platform_uv_executor_submit`
(`executor.c`), which pushes onto a double-buffered task list under a mutex and signals the
loop with `uv_async_send`. The async callback (loop thread) swaps the buffers and drains the
tasks. Submit vs. shutdown is serialized under the executor's recursive mutex.

### Asynchronous shutdown — `src/platform/uv/platform-uv.c`
`pomelo_platform_uv_close` starts an idle handle; `idle_close_cb` (`platform-uv.c`) closes
every manager (UDP, timer, executor, worker) plus the idle handle itself. Each manager
`uv_close`es its handles and, once drained, sets its bit in `closed_components`; when all
components are closed the user's close callback fires (`pomelo_platform_uv_check_closed`).
**Rule:** never free a handle's backing object before its `uv_close` callback has run.

---

## 4. End-to-end data flow

### Send (application → wire)
1. **API** — app writes a `pomelo_message_t` (wraps a delivery parcel), then
   `pomelo_session_send` (`src/api/session.c:41`) refs the message and dispatches through
   `channel->methods->send`.
2. **Delivery** — the channel's bus/endpoint (`src/delivery/bus.c`, `endpoint.c`) hands the
   parcel to a **dispatcher** (`dispatcher.c`), which splits it into **fragments**
   (`fragment.c`) sized to the fragment capacity, each carrying a packed meta
   (bus id, fragment index, last index, sequence).
3. **Protocol** — each fragment is emitted as a **PAYLOAD** packet; the protocol **sender**
   (`src/protocol/sender.c`) encrypts+authenticates it (ChaCha20-Poly1305-ietf) on a worker
   thread, then dispatches it.
4. **Platform** — `pomelo_platform_udp_send` (`src/platform/uv/udp.c`) performs the libuv
   send; the **adapter** `send_callback` unrefs the buffer when the write completes
   (`src/adapter/default.c`).

Reliable sends block the bus to one in-flight reliable parcel at a time (single dispatcher,
`src/delivery/bus.c`), ACKed per-fragment by the receiver.

### Receive (wire → application)
1. **Platform/Adapter** — libuv delivers a datagram; the adapter's alloc callback provides a
   pooled buffer and the recv callback wraps it into a view (`src/adapter/default.c`).
2. **Protocol** — the protocol socket reads the packet header, applies state checks + replay
   protection (`src/protocol/peer.c` `protect_replay`), then the **receiver**
   (`src/protocol/receiver.c`) decrypts the body on a worker thread and routes PAYLOAD
   contents to the delivery layer.
3. **Delivery** — `pomelo_delivery_bus_recv_fragment_data` (`src/delivery/bus.c`) validates
   the fragment meta, (re)acquires a **receiver** (`src/delivery/receiver.c`) sized to the
   parcel's fragment count, and reassembles; on completion it dispatches the parcel up.
4. **API** — the parcel surfaces as a `pomelo_message_t` delivered to the session's receive
   callback.

---

## 5. Handshake flow (protocol layer)

Secure, netcode-derived connect flow. Full wire detail in [STANDARD.md](STANDARD.md),
[PACKETS.md](PACKETS.md), [CONNECT_TOKEN.md](CONNECT_TOKEN.md).

```
  Client                               Server
    │  REQUEST  (connect token,           │
    │           public part + encrypted   │   decrypt private token with server key;
    │           private token, 1078 B) ─► │   allocate peer, set session keys
    │                                     │
    │  ◄─ CHALLENGE (encrypted challenge) │
    │                                     │
    │  RESPONSE (echo challenge token) ─► │   verify client_id + user_data match
    │                                     │   → CONNECTED
    │  ◄─ KEEP_ALIVE ─────────────────────│
    │  CONNECTED                          │
```

- **Peer states** (`src/protocol/peer.h`): `REQUEST(1) → CHALLENGE(3) → CONNECTED(4)`, plus
  negative terminal/timeout states. Every `recv_*` handler checks the exact expected state
  and ignores a packet that doesn't match — so out-of-order packets can't corrupt state.
- **Token gate:** session keys and `protocol_id` are copied into the peer's crypto context
  **only** from a successfully decrypted private connect token (`src/protocol/server.c`), so a
  peer without the server's private key can't advance past CHALLENGE.
- **Replay protection:** a 256-entry sliding window per peer (`src/protocol/peer.c`
  `protect_replay`), initialized to the `UINT64_MAX` empty sentinel.
- **Anti-DoS:** no amplification (REQUEST is full-size, responses are smaller); the number of
  half-open (pending) handshake peers is bounded against `max_clients`
  (`src/protocol/server.c`).

---

## 6. Where to make common changes

| Goal | Start here |
|------|-----------|
| New packet type / wire change | `src/protocol/packet.c` + [PACKETS.md](PACKETS.md) |
| Delivery semantics (reliability, fragmentation) | `src/delivery/bus.c`, `dispatcher.c`, `receiver.c` |
| Token / crypto | `src/crypto/token.c`, `crypto.c` + [CRYPTO.md](CRYPTO.md) |
| Public API surface | `include/pomelo/api.h`, `src/api/` + [API.md](API.md) |
| Platform / I/O behavior | `src/platform/uv/` + [PLATFORM.md](PLATFORM.md) |
| A plugin | [PLUGIN.md](PLUGIN.md), `src/api/plugin/` |
