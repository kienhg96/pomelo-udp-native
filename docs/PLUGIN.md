# Pomelo Plugin System (Agent Reference)

Extend the engine from a shared library via lifecycle hooks and custom transports. Orientation + build/test: [`/CLAUDE.md`](../CLAUDE.md). Cross-cutting model (threading, memory): [`ARCHITECTURE.md`](ARCHITECTURE.md). Public Socket/Session/Channel/Message surface: [`API.md`](API.md).

## Role

A plugin receives a `pomelo_plugin_t *` (a function-pointer table, `include/pomelo/plugin.h:133`) and registers callbacks to observe and alter engine behavior. Two uses: (1) **intercept** socket/session lifecycle events and attach custom data; (2) **implement a custom transport** — a plugin can create its own `pomelo_session_t` (type `POMELO_SESSION_TYPE_PLUGIN`), feed inbound messages up via `session_receive`, and take over outbound `session_send`. Plugin sessions wrap custom transports the same way builtin sessions wrap the delivery/protocol layers (see `ARCHITECTURE.md` §1). All callbacks run on the **loop thread**; only `set_data`/`get_data`, `session_set_private`/`session_get_private`, and the executor submit are thread-safe.

## File map

| File | Role |
|------|------|
| `src/api/plugin/plugin.c/.h` | Core: `pomelo_plugin_register`, callback storage, `configure`/`set_data`/`get_data`, the `dispatch_socket_on_*` fan-out loops, auto-release of acquired messages |
| `src/api/plugin/manager.c/.h` | `pomelo_plugin_manager_t`: owns the plugin `pomelo_list_t`, holds the method-table template (`tpl`), adds/destroys plugins |
| `src/api/plugin/loader.c` | `dlopen`/`LoadLibrary` the shared lib and resolve the `pomelo_plugin_initializer_entry` symbol (`load_by_name` / `load_by_path`) |
| `src/api/plugin/socket.c/.h` | Exported `socket_*` helpers: `socket_get_nchannels`, `socket_get_channel_mode`, `socket_time` |
| `src/api/plugin/session.c/.h` | Plugin-backed session impl: `session_create`/`destroy`/`receive`/`set_private`/`get_private`, methods table, disconnect/get_rtt forwarding |
| `src/api/plugin/channel.c/.h` | Plugin channel: `set_mode` → `session_set_mode` callback; `send` → `session_on_send` callback |
| `src/api/plugin/message.c/.h` | Exported `message_acquire`/`write`/`read`/`length` over `pomelo_message_t` |
| `src/api/plugin/token.c/.h` | `connect_token_decode`: decode public + private connect token (needs `socket->private_key`) into a `pomelo_plugin_token_info_t` |
| `src/api/plugin/executor.c/.h` | Thread-safe executor: `executor_startup`/`shutdown`/`submit` to run a task back on the loop thread |

## Lifecycle hooks

Registered in one call via `configure_callbacks` (`plugin.c:141`). **The argument order is NOT grouped by subsystem** — pass them exactly in this order (see `plugin.h:139`):

| # | Callback | Fired from | When |
|---|----------|-----------|------|
| 1 | `on_unload` | `pomelo_plugin_destroy` (`plugin.c:118`) | plugin/context teardown — free your data here |
| 2 | `socket_on_created` | `dispatch_socket_on_created` (`plugin.c:258`) ← `src/api/socket.c:130` | socket allocated |
| 3 | `socket_on_destroyed` | `dispatch_socket_on_destroyed` (`plugin.c:341`) ← `src/api/socket.c:772` | socket freed |
| 4 | `socket_on_listening` | `dispatch_socket_on_listening` (`plugin.c:275`) ← `src/api/socket.c:290` | server starts listening (gets `pomelo_address_t *`) |
| 5 | `socket_on_connecting` | `dispatch_socket_on_connecting` (`plugin.c:299`) ← `src/api/socket.c:236` | client starts connecting (gets raw `connect_token`) |
| 6 | `socket_on_stopped` | `dispatch_socket_on_stopped` (`plugin.c:324`) ← `pomelo_plugin_stop_socket` (`plugin.c:56`) | socket stopped |
| 7 | `session_on_send` | per-session `channel_plugin_send` (`channel.c:115`); broadcast `socket_send_plugin` (`src/api/socket.c:688`) | app sends on a plugin session — plugin owns the wire write |
| 8 | `session_disconnect` | `session_plugin_disconnect` (`session.c:278`) | app disconnects a plugin session |
| 9 | `session_get_rtt` | `session_plugin_get_rtt` (`session.c:294`) | app queries RTT (fill `mean`/`variance`) |
| 10 | `session_set_mode` | `channel_plugin_set_mode` (`channel.c:74`) | app changes a channel's delivery mode; return 0 / negative errno |

Every dispatch iterates the manager's plugin list with a `pomelo_list_iterator_t` and calls each non-NULL callback, then runs `pomelo_plugin_post_callback_cleanup` (`plugin.c:358`) to auto-release messages that callback acquired. Any callback may be `NULL`.

## Writing a plugin

A plugin is a shared library exporting one entry via `POMELO_PLUGIN_ENTRY_REGISTER` (`plugin.h:25`), which version-checks against `POMELO_PLUGIN_VERSION_HEX` (aborts on mismatch) then calls your `register` function. Load it with `pomelo_plugin_load_by_name`/`_by_path` + `pomelo_plugin_register` (`include/pomelo/api.h:607`).

```c
#include "pomelo/plugin.h"

typedef struct my_plugin_s { uint64_t messages_sent; } my_plugin_t;

static void on_unload(pomelo_plugin_t * plugin) {
    free(plugin->get_data(plugin));           // free whatever set_data stored
}

static void on_session_send(
    pomelo_plugin_t * plugin, pomelo_session_t * session,
    size_t channel_index, pomelo_message_t * message
) {
    my_plugin_t * self = plugin->get_data(plugin);
    self->messages_sent++;
    // read the outbound bytes and push them onto your own transport:
    size_t len = plugin->message_length(plugin, message);
    uint8_t buf[1200];
    plugin->message_read(plugin, message, buf, len);
    /* ... your_transport_send(buf, len) ... */
}

static void register_plugin(pomelo_plugin_t * plugin) {
    plugin->set_data(plugin, calloc(1, sizeof(my_plugin_t)));
    plugin->configure_callbacks(
        plugin,
        on_unload,           // 1  on_unload
        NULL,                // 2  socket_on_created
        NULL,                // 3  socket_on_destroyed
        NULL,                // 4  socket_on_listening
        NULL,                // 5  socket_on_connecting
        NULL,                // 6  socket_on_stopped
        on_session_send,     // 7  session_on_send
        NULL,                // 8  session_disconnect
        NULL,                // 9  session_get_rtt
        NULL                 // 10 session_set_mode
    );
}

POMELO_PLUGIN_ENTRY_REGISTER(register_plugin)
```

Key table methods (`plugin.h:133`), all via the `plugin` handle:

- **Data attach** — `set_data(plugin, ptr)` / `get_data(plugin)`: one void* per plugin, atomic/thread-safe (`plugin.c:173,187`). `session_set_private`/`session_get_private`: one void* per session (`session.c:20,35`).
- **Messages** — `message_acquire` returns a message **auto-released when the current callback returns** (tracked in `acquired_messages`, `message.c:7`); `message_write`/`message_read`/`message_length` (`message.c:28,46,65`). Keep nothing past the callback.
- **Custom transport** — `session_create(plugin, socket, client_id, address)` builds a `POMELO_SESSION_TYPE_PLUGIN` session, adds it to the socket, and fires the API `on_connected` (`session.c:51`); `session_destroy` defers teardown through the socket sequencer (`session.c:89`); `session_receive(plugin, session, channel_index, message)` packs the message and surfaces it to the app's receive callback (`session.c:112`).
- **Socket introspection** — `socket_get_nchannels`, `socket_get_channel_mode`, `socket_time` (`socket.c:11,25,47`).
- **Token** — `connect_token_decode(plugin, socket, connect_token, &info)` decodes the public then private part (using `socket->private_key`) into the fields of `pomelo_plugin_token_info_t` you set non-NULL (`token.c:10`).
- **Executor** — `executor_startup` acquires a platform executor (and pins the platform open), `executor_submit(plugin, cb, data)` is callable from **any thread** and runs `cb` back on the loop thread, `executor_shutdown` releases it (`executor.c:5,27,65`). Use this to hand work from a transport thread back onto the loop.

## Invariants & gotchas

- **Do NOT load or unload a plugin inside a lifecycle dispatch callback.** The dispatch loops iterate the plugin list with a `pomelo_list_iterator_t` (`plugin.c:266,286,310,332,349`). `pomelo_list_iterator_next` checks `it->mod_count == list->mod_count` (`src/utils/list.c:497-503`): mutating the list mid-iteration (registering/destroying a plugin) fails fast — `assert(false)` in debug, or returns `-2` and **stops the iteration early (remaining plugins are skipped)** in release. This is fail-fast, **not** memory-unsafe, but it is a hard constraint. (Noted in `REVIEW.md` §5.)
- **Callbacks run on the loop thread**, serialized by the sequencer (`ARCHITECTURE.md` §3). Don't block in them. Push blocking/foreign-thread work through `executor_submit`, which marshals back to the loop.
- **`configure_callbacks` order is positional and not subsystem-grouped** (unload, then the 5 socket hooks, then send, disconnect, get_rtt, set_mode). Getting the order wrong silently wires the wrong hook. See the table above / `plugin.h:139`.
- **Acquired messages live only for the callback.** `message_acquire` messages are auto-unref'd by `post_callback_cleanup` when the callback returns (`plugin.c:358`); never cache the pointer across calls.
- **`set_data`/`get_data` and the session private pointer are the only per-instance storage** — the plugin struct itself is a fixed method table copied from the manager template (`manager.c:77`); don't stash state in it.
