# Pomelo API Layer (Agent Reference)

> The public surface of the engine — Socket / Session / Channel / Message plus the
> plugin system. For orientation + build/test start at [`/CLAUDE.md`](../CLAUDE.md);
> for the cross-cutting memory/threading/data-flow model see
> [`docs/ARCHITECTURE.md`](ARCHITECTURE.md). Public header: `include/pomelo/api.h`.

## Role

The API layer is the only layer applications link against. It exposes four opaque
handles — **Socket**, **Session**, **Channel**, **Message** — and a **plugin**
registry, all declared in `include/pomelo/api.h`. A socket dispatches sends through
`channel->methods->send` (`src/api/channel.h:36`); the concrete implementation is
chosen per session type: **builtin** sessions (`src/api/builtin/`) wrap the
delivery + protocol layers (the real UDP transport), while **plugin** sessions
(`src/api/plugin/`) wrap custom transports supplied by a loaded plugin. The layer
owns no I/O itself — it is a thin, method-table-driven façade over Delivery/Protocol.

## Core concepts

- **Socket** (`src/api/socket.c`, `socket.h`) — client or server endpoint. Created
  from a `pomelo_context_t` with a fixed channel count + per-channel default modes;
  servers `listen`, clients `connect` with a connect token. Holds the session list,
  the protocol socket, the adapter, and its own `sequencer`.
- **Session** (`src/api/session.c`, `session.h`) — one established peer connection.
  Carries `client_id`, `address`, a monotonic atomic **signature** (stale-handle
  guard), and a `methods` table. Type is `BUILTIN` or `PLUGIN` (`session.h:55`).
- **Channel** (`src/api/channel.c`, `channel.h`) — a logical stream on a session with
  a delivery **mode** (`pomelo_channel_mode`, `include/pomelo/common.h:96`):
  `UNRELIABLE` (0, best-effort), `SEQUENCED` (1, ordered, may drop stale),
  `RELIABLE` (2, guaranteed + ordered). Mode is per-channel and can be changed at
  runtime via `pomelo_channel_set_mode`.
- **Message** (`src/api/message.c`, `message.h`) — a ref-counted, poolable buffer
  wrapping a delivery parcel. It is **mode-stateful**: `WRITE` (writer active) or
  `READ` (reader active); typed `write_*`/`read_*` helpers (`message.c:201-539`) are
  bounds-checked. `pomelo_message_reset` returns it to empty+writable.

**Usage flow:** create a context → `pomelo_socket_create` with channel config →
server `pomelo_socket_listen` / client `pomelo_socket_connect(token)` → on connect,
acquire a message (`pomelo_context_acquire_message`), write fields, then
`pomelo_session_send` / `pomelo_channel_send` / `pomelo_socket_send` (multicast) →
receive via the external `pomelo_socket_on_received` callback → query
`pomelo_session_get_rtt` → `pomelo_session_disconnect` / `pomelo_socket_stop` /
`pomelo_socket_destroy`.

Receive + connect/disconnect events are delivered through **external-linkage
callbacks** the application must define (`api.h:299-348`):
`pomelo_socket_on_connected`, `_on_disconnected`, `_on_received`,
`_on_connect_result`, `_on_send_result`.

## File map

| File | Responsibility |
|------|----------------|
| `src/api/context.c` / `context.h` | Root + shared `pomelo_context_t`; owns pools (socket/session/channel/message), buffer/protocol/delivery/plugin sub-contexts; `message_capacity`; message acquire/release |
| `src/api/socket.c` / `socket.h` | Socket lifecycle, listen/connect/stop, multicast send + builtin/plugin partitioning, `dispatch_send_result`, session list |
| `src/api/session.c` / `session.h` | Session façade: send, disconnect, get_channel, get_rtt, signature; base init/cleanup + method tables |
| `src/api/channel.c` / `channel.h` | Channel façade: send, set/get mode; method table |
| `src/api/message.c` / `message.h` | Message ref-counting, pack/unpack, prepare/finish send, typed bounds-checked write/read |
| `src/api/builtin/` | Builtin session/channel over Delivery+Protocol (`session.c`, `channel.c`, `delivery.c`, `protocol.c`) |
| `src/api/plugin/` | Plugin manager, loader (`dlopen`), plugin-backed session/channel/socket/message/token, dispatch hooks |

## Key types & entry points

- **`pomelo_context_t`** (`src/api/context.h:49`) — root (`pomelo_context_root_create`,
  `context.c:30`, optionally `synchronized`) vs shared (`pomelo_context_shared_create`,
  `context.c:337`, non-threadsafe child of a synchronized root). Holds all object
  pools + `message_capacity` (default 65536, `context.h:20`).
  `pomelo_context_acquire_message` (`context.c:499`) hands out a fresh WRITE message.
- **Socket**: `pomelo_socket_create` (`socket.c:166`; enforces
  `0 < nchannels <= POMELO_MAX_CHANNELS` = 1024, `constants.h:21`),
  `pomelo_socket_listen` (`socket.c:241`), `pomelo_socket_connect` (`socket.c:197`;
  both require state `STOPPED`), `pomelo_socket_send` multicast (`socket.c:325`),
  `pomelo_socket_destroy` (`socket.c:178`, deferred via sequencer).
- **Session**: `pomelo_session_send` (`session.c:41`), `pomelo_session_disconnect`
  (`session.c:87`, deferred), `pomelo_session_get_channel` (`session.c:114`),
  `pomelo_session_get_signature` (`session.c:75`), `pomelo_session_get_rtt`
  (`session.c:99`).
- **Channel**: `pomelo_channel_send` (`channel.c:26`), `pomelo_channel_set_mode`
  (`channel.c:42`), `pomelo_channel_get_mode` (`channel.c:58`). Method table:
  `pomelo_channel_methods_t` (`channel.h:36`).
- **Message**: `pomelo_message_write_buffer` (`message.c:201`, OVERFLOW gate at
  `message.c:219`), `pomelo_message_read_buffer` (`message.c:359`, UNDERFLOW at
  `message.c:375`), ref lifecycle `pomelo_message_ref`/`_unref` (`message.c:31`/`:39`),
  internal `prepare_send`/`finish_send`/`on_finalize` (`message.c:179`/`:189`/`:173`).

## Ownership & lifetime (IMPORTANT)

**Message ref-counting is the heart of send correctness.** A message is pooled and
ref-counted (`pomelo_reference_t`, finalize → pool). The send lifecycle:

1. Every public send entry (`pomelo_session_send`, `pomelo_channel_send`,
   `pomelo_socket_send`) first calls **`pomelo_message_prepare_send`**
   (`message.c:179`): ref **+1**, sets `POMELO_MESSAGE_FLAG_BUSY`, zeroes `nsent`,
   stores the callback `data`. While BUSY the message is locked — writes/reads
   return `POMELO_ERR_MESSAGE_BUSY`.
2. The terminal step is **`pomelo_socket_dispatch_send_result`** (`socket.c:583`).
   It is the **single place** that calls `pomelo_message_finish_send`
   (`message.c:189`: clear BUSY, unref **−1**) and then fires the external
   `pomelo_socket_on_send_result(socket, message, data, nsent)` callback.
3. When the last ref drops, `pomelo_message_on_finalize` (`message.c:173`) returns
   the message to its context pool.

**Invariant:** every send path — including all early-exit/failure paths — **must**
route through `dispatch_send_result`, or the `prepare_send` ref and the BUSY flag
leak (message never returns to pool, caller's send callback never fires). This is
exactly why the following early-outs each call it:
`pomelo_session_send` signature==0 path (`session.c:54-56`) **and** the invalid-channel
path (`session.c:62-67`, the merged fix); `pomelo_socket_send` invalid-channel /
no-sessions / no-connected / builtin-send-failure / no-builtin paths
(`socket.c:341,346,392,450,473`).

**Who owns the message after send:** the socket. Public doc contract states "the
message WILL BE managed by socket" (`api.h:247,373`). The caller keeps its own ref
only if it explicitly `pomelo_message_ref`'d before sending; otherwise it must treat
the handle as surrendered. On **receive**, the message handed to
`pomelo_socket_on_received` is released immediately after the callback returns
(`api.h:316`) — copy out anything you need.

**Stale-session guard:** `pomelo_session_cleanup` (`session.c:220`) stores signature
`0` when a session is released. `pomelo_session_send` loads the atomic signature
(`session.c:53`); a `0` means the caller holds a freed/recycled handle, so it
dispatches a failure result instead of dereferencing — no use-after-free.

## Invariants

- **Write bounds:** `written_bytes + length > context->message_capacity` →
  `POMELO_ERR_MESSAGE_OVERFLOW` (`message.c:219`). No over-write.
- **Read bounds:** reader underrun → `POMELO_ERR_MESSAGE_UNDERFLOW`
  (`message.c:375`). No over-read.
- **Mode gating:** writing a READ-mode message → `POMELO_ERR_MESSAGE_WRITE`; reading a
  WRITE-mode message → `POMELO_ERR_MESSAGE_READ`; writing/reading a BUSY message →
  `POMELO_ERR_MESSAGE_BUSY`.
- **`signature == 0` ⇒ released session.** Any API call that races against release is
  caught by the atomic signature load, not by locking.
- **Single-threaded by construction.** Every API operation runs on the owning loop
  thread, serialized by the socket's `sequencer` (`socket->sequencer`). Disconnect and
  destroy are *deferred* onto the sequencer (`session.c:89`, `socket.c:180`). Apparent
  races (signature-check vs. send, session-list mutation) are **false positives** — see
  [`ARCHITECTURE.md` §3](ARCHITECTURE.md).

## Gotchas

- **No plugin load/unload inside a dispatch callback.** The plugin dispatch loops
  (`src/api/plugin/plugin.c:258-338`) iterate the plugin list with a
  `pomelo_list_iterator_t`; `pomelo_list_iterator_next` asserts/validates
  `mod_count` (`src/utils/list.c:498`). Loading or unloading a plugin from within a
  lifecycle callback mutates the list mid-iteration → **fail-fast** (assert in debug,
  early `-2` return in release that skips remaining plugins). It is *not*
  memory-unsafe, but it is an unsupported re-entrancy — don't do it.
- **Builtin vs plugin session tags matter on multicast.** `pomelo_socket_send`
  partitions the `sessions[]` array in place: first filters to CONNECTED, then sorts
  builtin-to-front / plugin-to-back, dispatches each group separately
  (`pomelo_socket_send_builtin` `socket.c:624`, `pomelo_socket_send_plugin`
  `socket.c:670`), then restores the original order. It mutates the caller's array
  transiently via `tmp_original_index`.
- **Callback re-entrancy / surrendered handles.** `on_send_result` fires from inside
  `dispatch_send_result`; `on_received` releases the message right after returning.
  Do not retain raw `message`/`session` pointers past the callback without taking a
  ref, and do not assume a `session` handle cached across turns is still live — check
  `pomelo_session_get_signature`.
- **`pomelo_socket_get_channel_mode` returns the socket's *initial* default** for a
  channel index; a live session's channel may have been re-moded since.

## Plugin system

The plugin layer lets a custom transport masquerade as a builtin session. It lives in
`src/api/plugin/`: `manager.c` (registry), `loader.c` (`dlopen` of a shared object
exporting `pomelo_plugin_initializer_entry`), `plugin.c` (the `pomelo_plugin_t` API +
lifecycle dispatch), and plugin-backed `session.c` / `channel.c` / `socket.c` /
`message.c` / `token.c`. Lifecycle hooks (`socket_on_created/listening/connecting/
stopped`, session created/disconnected, etc.) are dispatched from `plugin.c`.
For the plugin authoring contract see [`docs/PLUGIN.md`](PLUGIN.md).
