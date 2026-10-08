# Pomelo Protocol Layer (Agent Reference)

> Secure, connection-oriented protocol over UDP: handshake, peer state, per-packet AEAD, replay protection, anti-DoS. Orientation + build/test: [`/CLAUDE.md`](../CLAUDE.md). Cross-cutting model (threading, memory, data flow, handshake §5): [`docs/ARCHITECTURE.md`](ARCHITECTURE.md).

## Role

The protocol layer turns an unreliable UDP datagram channel into authenticated, connection-oriented peer sessions. It runs the netcode-derived handshake (`REQUEST → CHALLENGE → RESPONSE → CONNECTED`), tracks per-peer connection state, and encrypts/authenticates every packet body with ChaCha20-Poly1305-ietf (per-packet AEAD). It enforces replay protection (per-peer sliding window) and anti-DoS limits (bounded half-open handshakes, no amplification). It sits above Platform/Crypto and below Delivery: decrypted `PAYLOAD` bodies are handed up, and the layer is agnostic to the fragment/parcel content inside them.

## File map

| File | Role |
|------|------|
| `socket.c/h` | Base socket: mode/state, recv/send entry points, incoming validation + receiver/sender dispatch, mode routing to server/client |
| `server.c/h` | Server mode: peer address map, 4 handshake lists, REQUEST/RESPONSE handling, challenge, keep-alive, deny, half-open reaping |
| `client.c/h` | Client mode: single server peer, REQUEST/RESPONSE emitters, CHALLENGE/DENIED/KEEP_ALIVE handling, connect timeouts |
| `peer.c/h` | Peer struct, state enum, replay protector, sender/receiver lists, deferred disconnect |
| `packet.c/h` | Packet type enum, header encode/decode, body-length validation, per-type encode/decode |
| `crypto.c/h` | Per-peer crypto context (ref-counted): per-packet AEAD encrypt/decrypt + associated-data construction |
| `receiver.c/h` | Inbound pipeline: worker-thread decrypt + decode, then completion back on loop thread |
| `sender.c/h` | Outbound pipeline: encode + worker-thread encrypt, adapter send, completion |
| `emitter.c/h` | Client-only frequency timer (retransmit REQUEST/RESPONSE at a Hz, with limit/timeout callbacks) |
| `context.c/h` | Shared pools (peers, packets, senders, receivers, crypto contexts, acceptances) + payload capacity |
| `adapter.c/h` | Platform↔protocol glue: wraps inbound datagrams into an `acceptance` task, buffer acquire |
| `protocol.h` | Internal public surface: context/server/client create, start/stop, `on_*` callbacks, opaque typedefs |

## Handshake & peer states

**States** — `src/protocol/peer.h:49-62`. Positive = in-progress/connected, non-positive = terminal/idle:

```
 CONNECTED (4)  ← fully connected
 CHALLENGE (3)  ← server: sent CHALLENGE, awaiting RESPONSE
 RESPONSE  (2)  ← client: got CHALLENGE, sending RESPONSE
 REQUEST   (1)  ← sending/awaiting the REQUEST stage
 DISCONNECTED (0)
 DENIED (-1) · REQUEST_TIMED_OUT (-2) · RESPONSE_TIMED_OUT (-3) · TIMED_OUT (-4)
 INVALID_CONNECT_TOKEN (-5) · CONNECT_TOKEN_EXPIRE (-6) · DISCONNECTING (-7)
```

Wire-level flow (bytes) is in [ARCHITECTURE.md §5](ARCHITECTURE.md#5-handshake-flow-protocol-layer); packet/token formats in [STANDARD.md](STANDARD.md), [PACKETS.md](PACKETS.md), [CONNECT_TOKEN.md](CONNECT_TOKEN.md).

**Server progression** (`server.c`): `validate` admits a new peer at `REQUEST` (`server.c:336`) → `recv_request` verifies slots + loads keys, moves to `CHALLENGE` (`server.c:490`) → `recv_response` verifies `client_id`+`user_data`, moves to `CONNECTED` (`server.c:551`).

**Client progression** (`client.c`): `REQUEST` (`client.c:553`) → `recv_challenge` → `RESPONSE` (`client.c:435`) → `recv_keep_alive` → `CONNECTED` (`client.c:491`).

**State gating is two-phased.** Each inbound packet is gated *twice* on the exact expected state:
1. Pre-decrypt, in the mode's `validate` (cheap reject before any worker crypto): server `server.c:258-292`, client `client.c:278-310`. Server also applies replay here for connected-only packet types (`server.c:285-287`).
2. Post-decode, in the `recv_*` handler, which re-checks state before mutating (`recv_request` `server.c:446`, `recv_response` `server.c:524`, `recv_challenge` `client.c:430`, `recv_keep_alive` `client.c:486`). Out-of-order or stale packets are silently dropped, so they cannot corrupt state.

## Key types & entry points

- **`pomelo_protocol_socket_t`** (`socket.h:110`) — base type; `mode` (SERVER/CLIENT, `socket.h:34`) and `state` (STOPPED/STOPPING/RUNNING, `socket.h:44`). `server_t`/`client_t` embed it as first member and are reached by cast.
- **Create/lifecycle** (`protocol.h`): `pomelo_protocol_context_create` (`protocol.h:148`), `..._server_create` (`:173`), `..._client_create` (`:181`), `..._socket_start/stop/destroy`. Up-calls to the owner: `on_connected`, `on_disconnected`, `on_received`, `on_connect_result` (`protocol.h:269-297`).
- **`pomelo_protocol_peer_t`** (`peer.h:65`) — per-connection state, `crypto_ctx`, `replay_protector`, sender/receiver lists, `flags`. Public accessors in `peer.c` (`client_id`, `address`, `send`, `extra`).
- **Packet types** (`packet.h:57-65`, 7 total): `REQUEST(0), DENIED(1), CHALLENGE(2), RESPONSE(3), KEEP_ALIVE(4), PAYLOAD(5), DISCONNECT(6)`. `KEEP_ALIVE` is the ping/keepalive (there is no separate PONG type).
- **Inbound entry path**: `pomelo_adapter_on_recv` (`adapter.c:58`) → queues an `acceptance` task on the sequencer → `pomelo_protocol_socket_accept` (`socket.c:479`) → `..._validate_incoming` (header decode + body-length + mode validate, `socket.c:529`) → acquire + submit receiver (`socket.c:506-524`) → `..._handle_receiver_complete` (`socket.c:434`) → `..._validate_packet` (`socket.c:583`, post-decode keep-alive `client_id` check) → `..._recv_packet` (`socket.c:197`).
- **Worker-thread crypto**: `worker_required[]` tables decide which packet types offload to the libuv worker threadpool. Decrypt side: `receiver.c:14-22` (REQUEST/CHALLENGE/RESPONSE require a worker). Encrypt side: `sender.c:17-25` (only CHALLENGE). The `entry` runs on a worker, `complete` returns on the loop thread (`receiver.c:204-210`, `sender.c:202-208`). Everything else runs inline on the loop thread.

## Invariants

- **Replay window initialized to the empty sentinel.** `received_sequence[256]` is `memset` to `0xff` (= `UINT64_MAX`) at peer alloc and cleanup (`peer.c:66-70`, `peer.c:131-135`); `protect_replay` treats `UINT64_MAX` as "slot empty" so sequence 0 is accepted exactly once (`peer.c:170-196`, window = `POMELO_REPLAY_PROTECTED_BUFFER_SIZE` 256, `peer.h:12`).
- **Token gate: session keys come only from a decrypted private token.** For an anonymous peer, `validate` zeroes `protocol_id` and both packet keys and copies only the server's `challenge_key`/`private_key` (`server.c:343-355`). `client_to_server_key`/`server_to_client_key` and the real `protocol_id` are installed into the peer's crypto context **only** inside `recv_request`, from the successfully decrypted token (`server.c:470-480`). A peer without the server's private key cannot advance past CHALLENGE.
- **State-check-before-act.** Every `recv_*` re-verifies the exact expected state before mutating peer state or lists (see "Handshake & peer states"); the socket send path also requires `RUNNING` socket + `CONNECTED` peer (`socket.c:341-347`).
- **`PROCESSING_RESPONSE` flag is set/cleared as a pair.** Set when a RESPONSE receiver is submitted (`socket.c:518-521`), cleared when that receiver completes (`socket.c:450-453`); `validate` rejects a second concurrent RESPONSE while the flag is set (`server.c:272-274`). Prevents two overlapping RESPONSE decode tasks racing the CHALLENGE→CONNECTED transition.
- **No amplification.** REQUEST must be full body size (`POMELO_PROTOCOL_PACKET_REQUEST_BODY_SIZE`, validated `packet.c:231-232`); the server's replies (CHALLENGE 308 B, DENIED 0-byte body) are ≤ the REQUEST, so a spoofed source cannot be used to amplify.
- **Pending (half-open) handshakes are capped.** Before allocating a peer for an unauthenticated REQUEST, the server rejects when `requesting_peers->size >= max_clients` (`server.c:320-322`). Connected slots are independently capped in `recv_request` (`server.c:450-454`).

## Gotchas

- **`validate_body_length` guards the HMAC underflow** (`packet.c:217-255`). For encrypted non-REQUEST packets it checks `body_length >= POMELO_HMAC_BYTES` before subtracting (`packet.c:223-227`) — a merged hardening fix. Without it the `size_t` subtraction would wrap; even wrapped, every `length == EXPECTED_SIZE` branch would still fail-closed, but the guard makes intent explicit. PAYLOAD requires `0 < length <= POMELO_PACKET_BODY_CAPACITY` (`packet.c:246-247`).
- **The peer is allocated before the token is decrypted** — bounded by the cap. `validate` only does a cheap public `protocol_id` compare (`server.c:304-310`), then allocates a peer and submits an XChaCha20 decrypt worker task *before* the token is authenticated. This is intentional (connectionless UDP), kept safe by the `requesting_peers` cap above and by clients retransmitting dropped REQUESTs.
- **Half-open lists are reaped by three different mechanisms** (don't assume a single timer cleans everything):
  - `challenging_peers` — reaped on a timer: `scan_challenging_peers` drops peers whose age exceeds `timeout_ns` with **no** DENIED sent (`server.c:1068-1095`, driven at `POMELO_ANONYMOUS_REMOVAL_FREQUENCY_HZ` = 1 Hz, `socket.h:24`).
  - `requesting_peers` — **not** age-reaped; a peer drains out when its async token decode completes (→ `recv_request` moves it to CHALLENGE, or `recv_request_failed`/`deny_peer` on bad token).
  - `denied_peers` — released only after the DENIED packet is actually sent (`sent_denied`, `server.c:635-651`).
- **Server ignores CHALLENGE and DENIED packet types** (`server.c:242-246`); client ignores REQUEST/RESPONSE/KEEP_ALIVE validation paths it doesn't expect (`client.c:280-310`).
- **Keep-alive carries `client_id`**, re-checked post-decode against the peer (`socket.c:606-620`); a connected peer with a mismatched `client_id` is treated as invalid.

## Data flow

**Receive (wire → delivery):**
1. `pomelo_adapter_on_recv` (`adapter.c:58`) refs the datagram buffer and submits an `acceptance` sequencer task (keeps all protocol state single-threaded).
2. `socket_accept` → `validate_incoming` (`socket.c:529`): socket-RUNNING check, min-length check, `header_decode` (`packet.c:687`), `validate_body_length`, then mode `validate` which does the **pre-decrypt state check + replay** (`server.c:226` / `client.c:252`).
3. A receiver is acquired and submitted (`socket.c:506-524`). `receiver_process` (`receiver.c:187`) offloads decrypt+decode to a worker when `worker_required[type]` (REQUEST/CHALLENGE/RESPONSE), else runs inline; decrypt uses the peer's `crypto_ctx` (`crypto.c:61`).
4. `handle_receiver_complete` (`socket.c:434`, loop thread): clears `PROCESSING_RESPONSE`, runs post-decode `validate_packet`, updates stats/`last_recv_time`, then `recv_packet` routes by type. PAYLOAD bodies surface via `on_received` → Delivery (`socket.c:256-275`).

**Send (application → wire):**
1. `socket_send` (`socket.c:330`): requires RUNNING socket + CONNECTED peer, bounds-checks views/length, assigns the next peer sequence, acquires a PAYLOAD packet, calls `dispatch` (`socket.c:623`).
2. `dispatch` acquires a sender; `sender_process` (`sender.c:189`) encodes the header+body and, unless `NO_ENCRYPT`, encrypts the body with per-packet AEAD — on a worker for CHALLENGE, inline otherwise (`sender.c:116-159`).
3. `sender_dispatch` (`sender.c:219`) calls `pomelo_adapter_send` (server also passes the peer address) → Platform `udp_send`. `sender_complete` reports back via `handle_sender_complete` (`socket.c:415`) and releases the sender.
