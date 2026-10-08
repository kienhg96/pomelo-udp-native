# Pomelo Delivery Layer (Agent Reference)

> Reliable/sequenced/unreliable delivery over the protocol layer: it splits application messages (parcels) into wire-sized fragments, reassembles them, ACKs and retransmits. Orientation + build/test: [`/CLAUDE.md`](../CLAUDE.md). Cross-cutting memory/threading model and the end-to-end send/receive walkthrough: [`docs/ARCHITECTURE.md`](ARCHITECTURE.md) §4.

## Role

Sits between the **API** layer (channels/sessions) and the **protocol** layer (per-packet AEAD + handshake). It takes a *parcel* (one logical message, held as an array of buffer *chunks*), splits it into *fragments* that each fit one PAYLOAD packet, and tags each fragment with a packed meta header (bus id, fragment index, last index, sequence). On receive it reverses the process: validates the meta, buffers fragments per sequence, verifies a whole-parcel checksum, and dispatches the rebuilt parcel up. Reliability (ACK + retransmit), ordering, and staleness are all handled here; the protocol layer below only sees opaque PAYLOAD bytes. Because fragments ride *inside* already-decrypted/authenticated PAYLOAD packets, any malformed fragment implies an already-connected (post-auth) peer.

## Delivery modes

`pomelo_delivery_mode` (`delivery.h:30`) — the enum value equals the data `fragment_type` (`fragment.h:61`; `pomelo_delivery_mode_from_fragment_type` is an identity cast, `fragment.h:114`):

- **UNRELIABLE** (`=0`): best-effort, no ACK, no ordering. Receiver has an RTT-scaled expiry (`receiver.c:234`, 100ms–1s); a parcel whose fragments never all arrive is reaped from the expiry heap. Lowest overhead — position/telemetry traffic.
- **SEQUENCED** (`=1`): ordered, drops stale. A fragment whose `sequence < last_recv_sequenced_sequence` is dropped on arrival (`bus.c:257`) and again at completion (`bus.c:314`); on success `last_recv_sequenced_sequence` advances (`bus.c:317`). No retransmit — only the newest parcel matters.
- **RELIABLE** (`=2`): guaranteed + ordered. Every fragment is ACKed per-fragment; the sender resends unacked fragments on an RTT timer (`dispatcher.c:298`, 10ms–100ms) until all are acked. The receiver waits forever (no expiry, `receiver.c:240`). **On `main` only one reliable parcel is in flight per bus at a time** (single dispatcher) — see Gotchas.

## File map

| File | One-line role |
|------|---------------|
| `delivery.h` | Internal layer header: mode enum, opaque types, option structs, reader/writer, all public prototypes, limits (`PARCEL_MAX_FRAGMENTS` 65536, default 222). |
| `internal.h` | Forward-declares `dispatcher`/`receiver` to break header cycles. |
| `context.c` / `context.h` | Context = owner of all object pools (parcel/dispatcher/sender/receiver/endpoint/bus/reception/transmission/heartbeat) + buffer context; `root` (synchronized parcel pool) vs `shared` (per-thread) variants. Holds `fragment_content_capacity`, `max_fragments`. |
| `endpoint.c` / `endpoint.h` | A connection's delivery face: array of buses + a system bus (id 0), RTT calculator, clock, heartbeat. Wire entry (`_recv`) and exit (`_send`); ping/pong, time-sync, reliable-capacity handshake. |
| `bus.c` / `bus.h` | One logical channel: send queue + reliable gate, receiver map/heap, ACK reply, stale/duplicate filtering, reorder dispatch. The per-channel state machine. |
| `dispatcher.c` / `dispatcher.h` | **Send side** of one parcel on one bus: builds fragments, emits them as PAYLOAD, runs the retransmit timer, consumes ACKs, reports completion. |
| `receiver.c` / `receiver.h` | **Recv side** of one parcel on one bus: sizes the fragment array, collects fragments, verifies checksum (worker thread), completes. |
| `sender.c` / `sender.h` | Multi-bus send batch: computes parcel checksum once, fans a parcel out to N buses as N dispatchers (`transmission` = one bus+mode target), aggregates results. |
| `parcel.c` / `parcel.h` | Ref-counted message = array of buffer-view `chunks`; reader/writer helpers; `set_fragments` rebuilds a parcel from received fragments. |
| `fragment.c` / `fragment.h` | Fragment struct (content view + `acked`) and the packed meta codec (`_meta_decode` / `_meta_encode`). |
| `heartbeat.c` / `heartbeat.h` | Shared periodic timer that drives each scheduled endpoint's ping at `PING_FREQUENCY` (10 Hz). |
| `clock.c` / `clock.h` | Client-side time-offset estimator (HIGH/MEDIUM/LOW sync levels) fed by pong samples. |

## Key types & entry points

- **`pomelo_delivery_endpoint_t`** (`endpoint.h:80`) — per-connection. Inbound: `pomelo_delivery_endpoint_recv` (`endpoint.c:64`) decodes meta, caps `last_index` against `max_fragments` (`endpoint.c:77`), resolves the bus (`bus_id==0` → system bus; else `bus_id-1` index into `buses`, `endpoint.c:83`), then `pomelo_delivery_bus_recv`. Outbound: `pomelo_delivery_endpoint_send` (external linkage, `delivery.h:347`) hands views to the protocol layer. `out_reliable_capacity` / `in_reliable_capacity` (`endpoint.h:129`) are negotiated via the ping/pong capacity handshake but only exercised by the parallel branch (see Gotchas).
- **`pomelo_delivery_bus_t`** (`bus.h:51`) — one channel. Send: `process_sending` → `_process_sending_deferred` (`bus.c:528`) drains `pending_dispatchers`. Recv data: `pomelo_delivery_bus_recv_fragment_data` (`bus.c:224`). Recv ACK: `pomelo_delivery_bus_recv_fragment_ack` (`bus.c:325`). Reorder/dispatch: `handle_receiver_complete` (`bus.c:278`) → `dispatch_received` (`bus.c:446`, routes system-bus parcels to ping/pong, user-bus parcels to `on_received`). Key fields: `incomplete_reliable_dispatcher`, `incomplete_reliable_receiver`, `last_recv_reliable_sequence`, `last_recv_sequenced_sequence`, `sequence_generator`.
- **`pomelo_delivery_dispatcher_t`** (`dispatcher.h:43`) — send side, pipeline `dispatch → complete` (`dispatcher.c:30`). `init` (`dispatcher.c:72`) builds the fragment array from parcel chunks and picks checksum mode (NONE / EMBEDDED / EXTRA, `dispatcher.c:118`). `_send` (`dispatcher.c:329`) encodes meta + emits each unacked fragment. `_recv_ack` (`dispatcher.c:413`) marks a fragment acked and advances when all are acked — **OOB-guarded** (`if (!fragment) return;`, `dispatcher.c:426`).
- **`pomelo_delivery_receiver_t`** (`receiver.h:26`) — recv side, pipeline `wait_fragments → verify_checksum → complete` (`receiver.c:27`). `init` (`receiver.c:71`) sizes `fragments` to `last_index+1` (`receiver.c:119`). `add_fragment` (`receiver.c:269`) indexes by `fragment_index` — **OOB-guarded** (`if (!fragment) return;`, `receiver.c:281`); it dedups already-filled slots and advances the pipeline when all slots fill.
- **`pomelo_delivery_sender_t`** (`sender.h:31`) — multi-bus batch, pipeline `update_checksum → dispatch → complete` (`sender.c:13`). `add_transmission` (`sender.c:166`) registers a bus+mode; `dispatch` (`sender.c:317`) spawns one dispatcher per transmission with `sequence = ++bus->sequence_generator` (`sender.c:304`).
- **`pomelo_delivery_parcel_t`** (`parcel.h:15`) — ref-counted (`parcel.c`), `chunks` = array of `pomelo_buffer_view_t`. Built on send via `append_chunk` (`parcel.c:92`); rebuilt on recv via `set_fragments` (`parcel.c:119`, trims a trailing empty EXTRA-checksum fragment). Read/write with `pomelo_delivery_reader_*` / `_writer_*`.
- **`pomelo_delivery_fragment_meta_t`** (`fragment.h:90`) — `{type, bus_id, fragment_index, last_index, sequence}`. Meta byte bit layout (`fragment.h:29`): bits 7-6 type, bit 5 bus_id width, bit 4 fragment_index width, bit 3 last_index width, bits 2-0 sequence width; each width field is `value+1` bytes. `last_index = total_fragments - 1`. Codec: `_meta_decode` (`fragment.c:24`), `_meta_encode` (`fragment.c:95`).

> Bus **id** ≠ bus **index**: id 0 is the reserved system bus (heartbeat/ping/pong); user bus index `i` has id `i+1` (`fragment.h:46`, `endpoint.c:90`).

## Invariants

- **Reliable sequences start at 1.** `++bus->sequence_generator` (`sender.c:304`) pre-increments from 0, so the first reliable sequence is 1. `last_recv_reliable_sequence` is initialized/reset to 0 (`bus.c:194`), a safe sentinel that no real parcel uses, so the first reliable parcel is never mistaken for a duplicate.
- **One in-flight reliable dispatcher per bus (main).** `process_sending_deferred` refuses to start another dispatcher while `incomplete_reliable_dispatcher` is set (`bus.c:533`); it is cleared only on completion (`bus.c:563`). Symmetrically one `incomplete_reliable_receiver` at a time on recv (`bus.c:242`, `bus.c:416`).
- **`fragment_index` is bounds-checked before array access.** `receiver.c:281` (recv) and `dispatcher.c:426` (ACK) both null-check the `array_get_ptr` result and discard the fragment rather than dereferencing — the decoder does **not** enforce `fragment_index <= last_index`, so these guards are the real defense (merged `fix/delivery-hardening`).
- **`last_index` is capped against `context->max_fragments`.** Checked twice: `endpoint.c:77` on decode and `bus.c:236` in `recv_fragment_data`, before the receiver allocates its `last_index+1` fragment array — prevents memory-amplification from a crafted oversized `last_index`.
- **Duplicate/late reliable fragments are re-ACKed, not reprocessed.** If a fragment's sequence equals `last_recv_reliable_sequence` and there is no active receiver, the parcel is already complete, so the bus just replies an ACK and returns (`bus.c:247`). This keeps a sender's retransmits from stalling after the receiver has moved on.
- **Checksum covers the whole parcel, computed once.** Only parcels with ≥2 chunks get a BLAKE2b checksum (`sender.c:269`); it is computed once on a worker thread and shared across all transmissions, and verified on a worker thread at recv (`receiver.c:380`). Single-chunk parcels carry no checksum.
- **Ref-count balance on fragment content.** `attach_content` refs the buffer (`fragment.c:192`), `cleanup` unrefs it (`fragment.c:14`); parcel chunks are refed in `set_fragments` (`parcel.c:145`).

## Gotchas

- **Head-of-line blocking on reliable (main).** The single-dispatcher model means a lost reliable parcel blocks every later parcel on that bus until it is retransmitted/acked. A **parallel** reliable mode (map of concurrent dispatchers/receivers keyed by sequence, bounded by `out/in_reliable_capacity`) exists on branch **`dev/reliable-parallel`** and is **not merged** — on `main`, treat `out_reliable_capacity`/`in_reliable_capacity` and the negotiated capacity as plumbing that the single-dispatcher path does not yet exploit.
- **Meta field widths are packed and variable.** `bus_id`, `fragment_index`, `last_index` are 1–2 bytes each; `sequence` is 1–8 bytes (`fragment.c:43`). Encode asserts the three index fields fit in ≤2 bytes (`fragment.c:112`). Don't assume fixed offsets — always go through the codec.
- **Receiver array is sized to `last_index+1`, from the *first* fragment seen.** Subsequent fragments must agree: `check_meta` rejects a fragment whose `last_index+1` differs from the established size (`receiver.c:195`). A crafted mismatch is discarded, not resized.
- **Sequenced drops happen in two places.** Stale is filtered both on fragment arrival (`bus.c:257`) and again when the parcel completes (`bus.c:314`), because a parcel can finish reassembly after a newer one already advanced `last_recv_sequenced_sequence`.
- **`verify_checksum` mutates the last fragment's length.** It slices `POMELO_CRYPTO_CHECKSUM_BYTES` off the tail of the last fragment (EMBEDDED mode) before hashing (`receiver.c:404`); parcels of <2 fragments skip verification entirely (`receiver.c:387`).
- **Reliable receivers never expire.** Only sequenced/unreliable receivers go on the expiry heap (`receiver.c:234`); a stuck reliable receiver is cleaned up only by `bus_stop` (`bus.c:154`), not by timeout.

## Data flow

**Send (parcel → wire).** API hands a parcel to a `sender` with one or more transmissions (bus + mode). The sender pipeline (`sender.c:13`): (1) `update_checksum` hashes all chunks on a worker thread (if ≥2 chunks); (2) `dispatch` creates one `dispatcher` per transmission, each assigned `sequence = ++bus->sequence_generator`, and pushes it onto the bus's `pending_dispatchers`; (3) the bus drains that queue (`bus.c:528`), gated so at most one reliable dispatcher runs at a time. Each dispatcher splits the parcel into fragments, encodes the packed meta (`fragment.c:95`), and emits each fragment via `pomelo_delivery_endpoint_send` as a protocol **PAYLOAD** packet. For reliable, it then arms an RTT-scaled resend timer and resends unacked fragments until every fragment is acked (`dispatcher.c:413` → `dispatcher.c:442`).

**Receive (wire → parcel).** Protocol delivers decrypted PAYLOAD bytes to `pomelo_delivery_endpoint_recv` (`endpoint.c:64`): decode meta → cap `last_index` → resolve bus → `pomelo_delivery_bus_recv` (queues a `reception` task on the sequencer, `bus.c:205`). The task runs `recv_fragment_data` (`bus.c:224`): re-cap `last_index`, apply mode filters (reliable sequence match / sequenced staleness / duplicate re-ACK), then `ensure_recv_command` finds-or-creates the per-sequence `receiver`. For reliable fragments the bus replies an ACK (`bus.c:269` → `reply_ack`, `bus.c:345`). `add_fragment` (`receiver.c:269`) drops the fragment into its `fragment_index` slot (OOB-guarded); when all slots fill, the receiver verifies the checksum on a worker thread (`receiver.c:380`) and completes. `handle_receiver_complete` (`bus.c:278`) rebuilds the parcel via `set_fragments` and dispatches it up — to the endpoint's ping/pong handler for the system bus, or to `pomelo_delivery_bus_on_received` for a user bus, then the parcel is unref'd. ACK packets take the separate `recv_fragment_ack` path (`bus.c:325` → `dispatcher.c:413`).
