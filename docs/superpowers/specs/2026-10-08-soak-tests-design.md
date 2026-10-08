# Soak / Endurance Tests — Design Spec

**Date:** 2026-10-08
**Status:** Approved (brainstorming)
**Branch:** `main` (local commits only, not pushed)

## Goal

Add long-running **soak (endurance) tests** that run the engine under sustained
load and assert it reaches a stable steady state — no slow resource growth, no
unbounded queue/pool creep, no connection-lifecycle leaks, no crash — over an
operator-chosen duration. These complement the existing 19 short CTest tests,
which prove *correctness* but terminate in milliseconds and cannot surface
slow-accumulation defects.

## Scope

Four standalone executables (per the approved decision):

1. `pomelo-soak-delivery` — delivery layer in isolation.
2. `pomelo-soak-protocol` — protocol layer in isolation.
3. `pomelo-soak-api` — API layer in isolation.
4. `pomelo-soak-engine` — whole-engine combo (all layers, concurrent).

**Not CTest-registered.** They are built behind an opt-in CMake option and run
manually (nightly / on-demand), because a useful soak run is minutes-to-hours
long and must not slow `ctest`.

## Why the statistic counters make this clean

Every field in `pomelo_statistic_t` is an **occupancy gauge** ("number of active
X"), not a cumulative total. Verified fields:

- `allocator.allocated_bytes` (live bytes)
- `protocol`: `senders, receivers, packets, peers, servers, clients, crypto_contexts, acceptances`
- `delivery`: `dispatchers, senders, receivers, endpoints, buses, receptions, transmissions, parcels, heartbeats`
- `api`: `messages, builtin_sessions, plugin_sessions, builtin_channels, plugin_channels`
- `buffer`: `buffers`

Because they are occupancy gauges, the steady-state invariant is: **after a
warmup, no gauge may climb monotonically beyond baseline + slack.** That single
invariant is what each soak asserts.

Getters (exact symbols):
- Full engine: `pomelo_context_statistic(context, &stat)` (`include/pomelo/api.h:173`)
- Protocol-only: `pomelo_protocol_context_statistic(protocol_ctx, &stat.protocol)` (`src/protocol/protocol.h:159`)
- Delivery-only: `pomelo_delivery_context_statistic(delivery_ctx, &stat.delivery)` (`src/delivery/delivery.h:210`)
- Buffer: `pomelo_buffer_context_statistic(buffer_ctx, &stat.buffer)`

## Architecture

New directory `test/soak-test/`:

- `soak-common.h` — shared harness (config, sampler, growth/progress guards,
  stop-timer, progress reporter). One focused unit.
- `soak-delivery.c`, `soak-protocol.c`, `soak-api.c`, `soak-engine.c` — the four
  drivers. Each owns its own load generator and picks which getter/counters it
  samples.

### `soak-common.h` — the harness (the valuable part)

A soak that merely runs and exits proves nothing. The harness provides the
measurement that gives it value:

**Config** (`pomelo_soak_config_t`), resolved from env with argv override:
- `duration_seconds` — env `POMELO_SOAK_SECONDS` (default `30`); `argv[1]`
  overrides.
- `warmup_seconds` — env `POMELO_SOAK_WARMUP_SECONDS` (default `3`).
- `sample_interval_ms` — fixed `250`.

**Sampler** (`pomelo_soak_sampler_t`): tracks, per watched counter, the
`baseline` (first post-warmup sample) and the `high_water` (max over the run).

**Guards, evaluated at teardown:**
1. **Growth guard** — for each watched occupancy counter:
   `high_water <= baseline + slack`. `slack` is a small absolute bound per
   counter (default `4`); for `allocated_bytes` the bound is a percentage
   (default `25%` over baseline). A monotonic climb → FAIL.
2. **Progress guard** — the driver reports an iteration/message counter; the
   harness asserts it exceeded a floor (so a stalled-but-leak-free run FAILs).
3. **Zero-leak** — standard `pomelo_statistic_*_check_resource_leak` on the
   relevant sub-statistics (aborts on leak), as the existing tests do.

**Stop-timer:** a one-shot platform timer fires at `duration_seconds` and
triggers graceful shutdown *inside* the loop (so handle close callbacks run and
no handle leaks — same discipline as the Task 22 watchdog). A second absolute
hang-guard (duration + margin) guarantees the process can never hang forever.

**Reporter:** every sample prints one line — `elapsed`, `iterations`, and the
live values of the watched counters — plus a final PASS/FAIL summary with the
baseline vs high-water table.

The harness uses `pomelo_check` from `test/pomelo-test.h` for the final
assertions (abort-on-failure → non-zero exit, so an operator or CI wrapper sees
the failure).

### The four drivers

| Driver | Load it generates | Getter + watched counters |
|---|---|---|
| `soak-delivery` | Two endpoints via the transporter-double pattern (as `delivery-test-single.c`); continuously send parcels, mixing RELIABLE / SEQUENCED / UNRELIABLE and varying payload size to force fragment split + reassembly each cycle. | `pomelo_delivery_context_statistic` + `pomelo_buffer_context_statistic`: `parcels, dispatchers, senders, receivers, transmissions, receptions, endpoints, buses, heartbeats, buffers, allocated_bytes`. |
| `soak-protocol` | Real client + server over UDP loopback; **churn loop**: connect → exchange N payloads → disconnect, repeatedly, plus steady keep-alive traffic. | `pomelo_protocol_context_statistic` + buffer: `peers, senders, receivers, packets, crypto_contexts, acceptances, buffers`. Peers/half-open must recycle to baseline each cycle. |
| `soak-api` | Socket server + client session(s); channels in all three modes; continuous `session_send` on each mode + periodic `socket_send` broadcast. | `pomelo_context_statistic` (whole struct): `api.*`, `delivery.*`, `buffer.buffers`, `allocated_bytes`. |
| `soak-engine` | Full stack, several concurrent client sessions: some sustain mixed-mode traffic + broadcast while others churn connect/disconnect — everything at once. | `pomelo_context_statistic` — the entire `pomelo_statistic_t`. |

## Build & run model

- New CMake option `POMELO_BUILD_SOAK` (default **OFF**). When ON, the four
  targets build; none is registered with `add_test`. Build stays lean by
  default.
- Link each driver like the matching existing test target (e.g. `soak-delivery`
  mirrors `pomelo-test-delivery-single`'s link set; `soak-protocol` mirrors the
  server test's; `soak-api`/`soak-engine` mirror the api test link set),
  plus `TEST_STATISTIC_CHECK`.
- Run manually: `cmake -S . -B build -DPOMELO_BUILD_SOAK=ON && cmake --build build -j`
  then `POMELO_SOAK_SECONDS=300 ./build/pomelo-soak-engine` (or `./build/pomelo-soak-engine 300`).

## Success criteria (PASS/FAIL)

A soak PASSES iff all hold:
- **(a)** no watched counter's `high_water` exceeds `baseline + slack` after warmup;
- **(b)** forward-progress counter exceeded its floor;
- **(c)** zero-leak check passes at teardown;
- **(d)** no crash / no hang (process exits 0 within duration + margin).

Any failure aborts with non-zero exit and a diagnostic naming the offending
counter (baseline, high-water, slack) or the failed guard.

## Out of scope

- CTest registration (explicitly manual per decision).
- Sanitizer builds, CI wiring, coverage, fuzzing (tracked separately as
  out-of-review follow-ups).
- A dedicated replay/sequence-longevity soak — folded into `soak-protocol`'s
  sustained traffic rather than a 5th binary (YAGNI); can be split out later if
  needed.

## Constraints

- Local commits only — **DO NOT push**. Commit footer
  `Co-Authored-By: claude-flow <ruv@ruv.net>`.
- Single libuv loop thread + sequencer model applies; load generators drive via
  platform timers, never background threads.
