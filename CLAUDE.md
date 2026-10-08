# CLAUDE.md — Agent Guide for Pomelo UDP Native

> `README.md` is for **end users**. This file is for **agents** working in the code.
> Deep detail lives in [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) and the per-layer docs linked at the bottom.

## What this is

A high-performance UDP networking engine for game clients/servers. It implements a
secure, connection-oriented protocol over UDP — **Pomelo**, derived from
[netcode.io](https://github.com/networkprotocol/netcode) 1.02. Built on **libuv**
(async I/O) and **libsodium** (crypto). Language: C11, build: CMake.

- **On-wire protocol version string:** `"POMELO 1.00"` (`src/base/constants.h`, `POMELO_VERSION_INFO`). This is the 13-byte version embedded in connect tokens and the request packet — do **not** confuse it with the library version.
- **Library version:** `0.0.1` (`include/pomelo/version.h`, CMake `project(... VERSION 0.0.1)`).

## Build & test

Dependencies are git submodules (`deps/libuv`, `deps/libsodium`):

```bash
git submodule update --init --recursive      # once
cmake -S . -B build                           # configure
cmake --build build -j                        # build (lib + tests + examples)
ctest --test-dir build --output-on-failure    # run all tests
```

- CMake options: `POMELO_BUILD_TESTS` (ON), `POMELO_BUILD_EXAMPLES` (ON), `POMELO_BUILD_GENERATOR` (ON).
- Tests are plain C executables registered with CTest under `test/<area>-test/`. The harness is `test/pomelo-test.h`: `pomelo_check(cond)` aborts on failure, `pomelo_run_test(fn)` runs an `int fn(void)`.
- Build a single test: `cmake --build build --target pomelo-test-<area> && ctest --test-dir build -R pomelo-test-<area>`.

## Layer map (bottom-up — the order to understand the system)

| Layer | Dir | Responsibility | Key files | Doc |
|-------|-----|----------------|-----------|-----|
| **Crypto** | `src/crypto/` | Connect tokens (XChaCha20-Poly1305), per-packet AEAD (ChaCha20-Poly1305-ietf), checksum (BLAKE2b), base64, CSPRNG | `token.c`, `crypto.c`, `checksum.c`, `random.c` | [CRYPTO.md](docs/CRYPTO.md) |
| **Platform** | `src/platform/uv/` | libuv abstraction: UDP socket, timer, worker threadpool (crypto offload), cross-thread executor, time | `udp.c`, `timer.c`, `worker.c`, `executor.c`, `platform-uv.c` | [PLATFORM.md](docs/PLATFORM.md) |
| **Protocol** | `src/protocol/` | Secure handshake (REQUEST→CHALLENGE→RESPONSE→CONNECTED), peer/connection state, per-packet encryption, replay protection, anti-DoS | `socket.c`, `server.c`, `client.c`, `peer.c`, `packet.c`, `crypto.c`, `receiver.c`, `sender.c` | [PROTOCOL.md](docs/PROTOCOL.md) |
| **Delivery** | `src/delivery/` | Reliable/Sequenced/Unreliable delivery over the protocol; parcel ↔ fragment split/reassembly; ACK; retransmit | `endpoint.c`, `bus.c`, `dispatcher.c`, `receiver.c`, `sender.c`, `parcel.c`, `fragment.c` | [DELIVERY.md](docs/DELIVERY.md) |
| **API** | `src/api/`, `include/pomelo/api.h` | Public Socket/Session/Channel/Message; plugin system (`plugin/`) + builtin impl (`builtin/`) | `socket.c`, `session.c`, `channel.c`, `message.c`, `context.c` | [API.md](docs/API.md) |

Supporting code: **Adapter** (`src/adapter/`) bridges platform↔protocol for recv/send (buffer acquire + crypto glue); **Base** (`src/base/`) holds ref-counting, buffer, payload, pipeline, sequencer, allocator, address; **Utils** (`src/utils/`) holds list, array, heap, map, pool, mutex, atomic, rtt. Plugin dev → [PLUGIN.md](docs/PLUGIN.md). Wire formats → [PACKETS.md](docs/PACKETS.md), [CONNECT_TOKEN.md](docs/CONNECT_TOKEN.md), [STANDARD.md](docs/STANDARD.md).

## Core models (full detail in [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md))

- **Memory:** atomic **lock-free ref-counting** (`pomelo_reference`, `src/base/ref.c`) + **object pools** (`src/utils/pool.c`) + an **allocator abstraction** (`src/base/allocator.c`). Buffers, parcels, and messages are ref-counted and shared across the worker-thread boundary — always balance `*_ref`/`*_unref`.
- **Threading:** one **libuv loop thread per platform** owns nearly all state. Work is serialized through a **sequencer** (`src/base/sequencer.c`) — each task runs to completion before the next starts. Only **crypto is offloaded** to the libuv worker threadpool (`src/platform/uv/worker.c`); results return to the loop thread via completion callbacks. Other threads submit work to the loop through the **executor** (`uv_async`, `src/platform/uv/executor.c`).

## Gotchas for agents

- **Not concurrent where it looks concurrent.** Because of the single loop thread + sequencer, most state is touched by exactly one thread. Apparent "data races" on `peer->state`, sessions, etc. are usually false positives. Real concurrency is limited to: crypto on worker threads (operating on isolated buffers), and cross-thread `executor` submits.
- **libuv handles close asynchronously.** `uv_close(handle, cb)` finishes on a later loop iteration; never free the backing object before its close callback runs. Shutdown is orchestrated by closing each manager, then firing the user callback once a component bitmask is complete (`src/platform/uv/platform-uv.c`).
- **Validate external input at boundaries.** Wire-supplied lengths/indices (token address count, fragment index/last_index, packet body length) must be bounds-checked before use.
- **Encryption layering.** The protocol layer encrypts/authenticates whole packets (per-packet AEAD); delivery fragments ride *inside* decrypted PAYLOAD packets — so a malformed fragment implies an already-authenticated (connected) peer.

## Docs index

- [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) — cross-cutting: memory model, threading model, end-to-end data flow, handshake flow.
- Per layer: [CRYPTO](docs/CRYPTO.md) · [PLATFORM](docs/PLATFORM.md) · [PROTOCOL](docs/PROTOCOL.md) · [DELIVERY](docs/DELIVERY.md) · [API](docs/API.md) · [PLUGIN](docs/PLUGIN.md)
- Wire-format spec: [PACKETS](docs/PACKETS.md) · [CONNECT_TOKEN](docs/CONNECT_TOKEN.md) · [STANDARD](docs/STANDARD.md)
