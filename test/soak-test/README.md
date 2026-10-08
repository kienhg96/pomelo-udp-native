# Soak / endurance tests

Standalone, long-running endurance binaries. **Not** registered with CTest —
build with `-DPOMELO_BUILD_SOAK=ON` and run manually (nightly / on-demand).

| Binary | Scope |
|--------|-------|
| `pomelo-soak-delivery` | Two delivery endpoints under continuous mixed-mode parcel traffic (one parcel in flight at a time). |
| `pomelo-soak-protocol` | Protocol client↔server over UDP loopback, sustained echoed PAYLOAD traffic. |
| `pomelo-soak-api`      | One server + one client session, sustained mixed-mode `session_send`. |
| `pomelo-soak-engine`   | One server + 4 concurrent client sessions, full-stack mixed-mode traffic. |

## Build & run

```bash
cmake -S . -B build -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DPOMELO_BUILD_SOAK=ON
cmake --build build -j
POMELO_SOAK_SECONDS=300 ./build/pomelo-soak-engine      # 5-minute run
./build/pomelo-soak-delivery 600                        # argv overrides seconds
```

Env: `POMELO_SOAK_SECONDS` (default 30), `POMELO_SOAK_WARMUP_SECONDS` (default 3;
clamped to `duration/2` for short runs so the baseline is taken after warmup).

## What they assert

The shared harness `soak-common.h` samples the occupancy gauges in
`pomelo_statistic_t` every 250 ms. Every gauge is a "number of active X" counter,
so at steady state it must return to its baseline. After a warmup each soak
asserts three things:

1. **Growth guard** — no watched gauge's high-water exceeds `baseline + slack`
   (for `allocated_bytes`, `baseline + pct%`). A monotonic climb = a leak.
2. **Progress guard** — a forward-progress counter exceeded its floor (a stalled
   but leak-free run still fails).
3. **Zero-leak** — the standard `pomelo_statistic_*_check_resource_leak` at
   teardown.

A PASS prints `[soak] ==== PASS ====` and exits 0; any failure aborts non-zero
with a diagnostic naming the offending gauge or guard.

## Known findings & follow-ups

- **`pomelo-soak-delivery` is lossless on purpose.** Dropping packets in the
  transporter exercises the reliable *retransmit* path, which on `main` (the old
  single-dispatcher reliable model) intermittently trips the assert
  `incomplete_reliable_receiver == NULL` (`src/delivery/bus.c:416`) when a delayed
  retransmit races a just-completed reliable receiver. This is a pre-existing
  delivery issue — being superseded by the reliable-parallel model on the dev
  branch — not a soak-harness fault. A lossy/retransmit soak is deferred until
  that path is fixed.
- **`pomelo-soak-engine` ships persistent-only.** Connection churn
  (disconnect/reconnect cycling) is deferred; the 4-session concurrent combo is
  the primary full-stack coverage.
- Building these soaks surfaced and fixed two real latent bugs in
  `pomelo_list_iterator_transfer` (mod_count desync + missing debug signature
  stamp) — see commit `fix(utils): sync mod_count + stamp signature ...` and the
  regression test `pomelo_test_list_transfer_integrity`.
