# Pomelo UDP Native — Unit Test Quality Review

> Agent-facing audit of `test/`. Scope: **format correctness** (do tests follow the `pomelo-test.h` harness) and **use-case coverage quality** (do they verify the right behaviors, including regression coverage for the hardening fixes merged on `main`). Companion to [REVIEW.md](../REVIEW.md) (source review). No test files were modified by this review.
>
> Reviewed on 2026-10-08 against `main`. Every High/Med finding below was verified against the actual test + source (`file:line` anchored).

## Harness contract (what "correct format" means)

`test/pomelo-test.h`: a test is `static int test_xxx(void)` returning `0` on success; assertions use `pomelo_check(cond)` (aborts on failure); each test is registered in `main()` via `pomelo_run_test(test_xxx)`. Leak-based teardown asserts `pomelo_allocator_allocated_bytes(allocator)` returns to its captured baseline. Unit-style files (base/utils/crypto/packet) follow this; the networked files (platform/protocol/delivery/api/plugin) are **integration drivers** — a single scripted `main()` + `uv_run` with callbacks acting as test doubles. That style is acceptable, but it introduces the dominant risk below.

## Test inventory & CTest registration

| Test target | File(s) | CTest? | Notes |
|---|---|---|---|
| base | `base-test/*` | ✅ | 4/4 registered |
| utils | `utils-test/*` | ✅ | 6/6 registered |
| crypto | `crypto-test.c` | ✅ | |
| platform-uv | `platform-test/*` | ✅ | one mega-test |
| protocol (encrypted) | `protocol-test.c` | ✅ | real default adapter |
| protocol (unencrypted) | `protocol-test.c` + `-DPOMELO_ADAPTER_DEFAULT_NO_ENCRYPTION` | ✅ | same source, AEAD skipped |
| protocol-client | `client-test.c` + `adapter-simulator.c` | ✅ | |
| protocol-server | `server-test.c` + `adapter-simulator.c` | ✅ | |
| protocol-packet | `packet-test.c` | ✅ | encode/decode roundtrips |
| delivery-single | `delivery-test-single.c` | ✅ | |
| delivery-multiple | `delivery-test-multiple.c` | ✅ | |
| api-basic | `api-basic-test.c` | ✅ | |
| api-broadcast | `api-broadcast-test.c` | ✅ | |
| plugin | `plugin-test.c` + `demo-plugin.c` (.so) | ✅ | |
| demo-plugin | `demo-plugin.c` | — | loadable .so dep of plugin-test (by design) |
| **webrtc-plugin** | `webrtc-plugin-test.c` | ❌ | **built but never `add_test`'d** — dead/unrun |

Total: **14** CTest tests (matches the `14/14 pass` baseline).

Severity legend: **High** = test is wrong or verifies nothing meaningful / a shipped hardening fix has zero coverage · **Med** = weak assertion or notable gap · **Low** = style/convention.

---

## Cross-cutting themes (read these first)

### T1 — "Vacuous pass / hang" in the integration drivers (High, systemic)
The networked tests put their real assertions **inside callbacks** (`on_connected`, `on_received`, …) but `main()` only asserts **resource-leak** equality after `uv_run` returns. There is no post-run flag asserting the callback actually fired. Failure modes:
- If the exchange silently never completes, `uv_run` drains → leak checks pass → **"Test passed!"** while nothing about the protocol was verified; **or**
- a still-active handle keeps `uv_run` from returning → the test **hangs** and only CTest's timeout surfaces it (there is no per-test timeout in `pomelo_test_platform_run`, `platform-uv.c:26`).

Affected: `protocol-test.c`, `client-test.c`, `server-test.c`, `api-basic-test.c`, `api-broadcast-test.c`, `plugin-test.c`. **Fix pattern:** add a `volatile int completed`/`recv_counter` set in the callback and `pomelo_check(completed)` after `uv_run`. `delivery-test-multiple.c:333` already does this correctly — use it as the model.

### T2 — Hardening fixes have (almost) no regression coverage (High)
Of the 6 fixes merged on `main`, only **one** is covered:

| Hardening fix | Regression test? |
|---|---|
| Crypto `naddr` overflow (`token.c`) | ✅ `crypto-test.c:23` |
| Crypto bad-version reject | ✅ `crypto-test.c:76` |
| Crypto full-seed determinism | ✅ `crypto-test.c:92,105` |
| Platform idle-handle leak (`uv_close`) | ✅ `platform-test.c:429` (`uv_loop_close()==0`) |
| Platform executor submit-vs-shutdown mutex | ❌ none (executor only submitted **same-thread** — see P-PLATFORM) |
| Protocol half-open peer cap (`server.c:320`) | ❌ none (no `>max_clients` REQUEST flood) |
| Protocol body-length underflow guard (`packet.c:224`) | ❌ none (no malformed/short packet decode test) |
| Delivery `last_index` cap (`bus.c:236`/`endpoint.c:77`) | ❌ none |
| Delivery NULL-fragment guard (`receiver.c:281`) | ❌ none |
| API invalid-channel `dispatch_send_result` (`session.c:62`) | ❌ none (no send on out-of-range channel) |

---

## Crypto — `crypto-test.c`

- **🐞 High — Vacuous decrypt check.** `crypto-test.c:198` calls `pomelo_crypto_decrypt_aead(...)` **without capturing its return**; the following `pomelo_check(ret == 0)` at `:208` re-checks the **stale `ret` from the encrypt call** at `:182`. The decrypt success is never actually asserted (only an indirect `memcmp` catches total garbage). Capture and check the decrypt return.
- **High — No negative AEAD path.** The core security property — *decrypt fails closed on a tampered ciphertext/tag/AAD or wrong key* — is never tested. Only one happy-path encrypt→decrypt exists (the buggy block above). Add: flip a ciphertext byte, flip an AAD byte, use a wrong key → each must return `< 0`.
- **Med — `decode_private` wrong-key/tamper not tested.** Only the happy roundtrip (`:116`). Add a wrong-key and a corrupted-ciphertext case expecting rejection.
- **Med — Zero coverage: BLAKE2b checksum, base64, challenge-token codec.** Project-wide grep confirms **no** test anywhere for `pomelo_crypto_checksum_*`, `base64`, or `encrypt/decrypt_challenge_token`. base64 has a documented padding bug (CRYPTO.md) that nothing guards.
- **Low — Trailing AEAD block lives in `main()`** using file-scope globals instead of a named `pomelo_run_test` function; inconsistent with the rest of the file.

Good: regression tests for naddr overflow, bad version, and deterministic-seed fixes are present and correct.

---

## Base & Utils

All declared tests are registered; `pomelo_check` used correctly; harness followed. Gaps:

- **High — `ref-test.c`: the lock-free refcount's whole point is untested.** The CAS "refuse to revive a finalized object" path (`ref.c:16-35`: `ref()` returns `false` / `unref()` early-returns after count hits 0) is never exercised (`ref-test.c:16-32`). Caveat: debug builds `assert(false)` there, so covering it needs an NDEBUG build or redesign — flag accordingly.
- **High — `map-test.c`: no collision, no rehash/growth.** Keys stay ≤2 and land in distinct buckets, so the probe-chain, chained removal, and resize paths of the 733-line `map.c` are never run. Add colliding keys + enough inserts to force a rehash + an iterator test.
- **🐞 Med — `pool-test.c:73` wrong-variable assertion.** After acquiring `data2` at `:71`, the test asserts `*data == 2` (stale first pointer) instead of `*data2 == 2`, so the second-acquire init path is effectively unverified. Also: `acquire_batch`/`release_batch` tests are commented out (`:88-93`); `zero_init` untested.
- **Med — `allocator-test.c:26` absolute-value assert.** Asserts `allocated_bytes == 128` absolutely (relies on the global default allocator starting at 0) instead of snapshotting a baseline like the utils tests. Fragile to test-order. Also `realloc` byte-accounting (`allocator.c:138-189`), `malloc(0)`, and the failure callback are untested.
- **Med — `payload-test.c` packed-uint64 tests one value only** (`:93-109`), so the 1–7-byte length branches of `calc_packed_uint64_bytes` and `read_packed_uint64` underflow (`payload.c:618`) are untested.
- **Med — `address-test.c` only tests `from_string`.** `to_string`, `compare`, `hash`, `sockaddr` round-trips, etc. are untested (parsing itself is decently covered).
- **Low — `array-test.c:33-34`** shrink test doesn't verify surviving elements; grow zero-fill path untested. `heap-test.c` only removes the min node (interior-node sift paths untested).

Clean: `base-test.c`, `utils-test.c`, `list-test.c` (strong).

---

## Platform — `platform-test.c`

- **🐞 High — Timer-interval assertion is vacuous (ns/ms unit mismatch).** `delta = pomelo_platform_hrtime() - last` is in **nanoseconds** (`time.c:8` → `uv_hrtime()`), but it's compared `delta < TEST_TIMER_INTERVAL` where `TEST_TIMER_INTERVAL == 100` is **milliseconds** (`platform-test.c:139`). Real delta ≈ 1e8 ns, so `delta < 100` is never true → the "timer fired too early" branch (`:140`) is **dead code**. The test proves the callback fires 5× but never validates the interval. (That bad branch also only `printf`s — it sets no fail flag.)
- **High — Executor cross-thread submit never exercised.** The job is submitted from the main thread **before** `uv_run` (`:422` then `:424`) — the same thread that runs the loop. The executor's reason to exist (cross-thread `uv_async` submit) and the **submit-vs-shutdown mutex hardening** (T2) have zero coverage. Needs a real foreign-thread submit.
- **Med — Weak/near-vacuous executor data assert.** `temp_data` is `NULL` (`:41`), submitted as the job pointer, and the callback checks `data == temp_data` ⇒ `NULL == NULL` ⇒ always true (`:375`). Use a distinct sentinel pointer.
- **Med — Worker-cancel test is racy by construction.** Submit-then-immediately-cancel (`:100-110`) assumes the threadpool hasn't dequeued the entry; if it has, `test_work_cancel_result` stays 0 and the final assert (`:436`) flakes.
- **Med — UDP `niovec`/length-clamp + adapter NULL-iovec guard uncovered.** Every send uses `niovec==1`; scatter-gather and the clamp (`udp.c`) and the `adapter/default.c:219` NULL-base recv guard are never driven.
- **Low — Duplicate assertion** `pomelo_check(test_timer_result == 1)` twice (`:437` and `:438`); typo `tesk_main_task_result`.

Good: `pomelo_check(uv_loop_close(...) == 0)` (`:429`) is a correct regression test for the idle-handle leak fix — the strongest part of the suite.

---

## Protocol — `protocol-test.c`, `client-test.c`, `server-test.c`, `packet-test.c`

- **High — `client-test.c` / `server-test.c` pass vacuously (T1).** `main()` tails assert only `alloc_bytes == allocated_bytes(...)` + leak stats (`client-test.c:607-609`, `server-test.c:575-577`). No flag confirms `on_connected`/`on_received` or the `v1==sample_v1` value checks ever ran; `on_connect_result` only `printf`s (never asserts `SUCCESS`). A silently-broken handshake reports "Test passed!".
- **High — Named hardening fixes untested (T2).** No regression test for the half-open peer cap (`server.c:320`; no `>max_clients` REQUEST flood) or the body-length underflow guard (`packet.c:224`; no truncated/malformed decode). `validate_body_length` and `replay` appear in **zero** test files.
- **High — Replay protection never exercised.** No duplicate-sequence packet is sent to confirm the 256-entry window rejects/re-ACKs.
- **Med — `protocol-test.c` payload roundtrip not backstopped.** Handshake completion is correctly gated (`:228 connected_count == 2`), but the payload echo check lives only in the client `on_received` (`:329-335`) with no `received_count` asserted in `main()`.
- **Med — Rejection paths uncovered:** bad/corrupt token, wrong private key, expired token, version/`protocol_id` mismatch, wrong-state packets (PAYLOAD before CONNECTED). None tested across the suite.
- **Med — Inbound DISCONNECT / KEEP_ALIVE timeout** never driven over the wire (e2e tests disconnect via the local API; timeouts are disabled: `client-test.c:26 = -1`, `server-test.c:28 = 1s` but handshake finishes first).
- **Low — `packet-test.c` style/latent issues:** uses `pomelo_check(test_x()==0)` instead of `pomelo_run_test` (`:756-762`); DISCONNECT test reuses the `denied_info`/`denied` structs against the DISCONNECT pool (`:658-665`, works only because bodies coincide); `read_uint64` return unchecked (`:631`); `naddresses = rand % MAX` can be 0 → address-roundtrip loop skipped non-deterministically (`:125`).

Good: all **7** packet types have encode/decode roundtrips in `packet-test.c`; the encrypted/unencrypted dual-build mechanism is sound.

---

## Delivery — `delivery-test-single.c`, `delivery-test-multiple.c`

- **🐞 High — `delivery-test-single.c` double-counts.** `total_transmission_count += transmission_count;` appears **twice** in the same `on_result` callback (`:125` and `:141`), inflating the counter. Not a false-green today (the `received_reliable_parcels` gate at `:66` still holds) but the count is wrong and weakens the assertion. Remove one.
- **High — Delivery hardening fixes untested (T2).** No test crafts a fragment meta with `last_index >= max_fragments` (the `bus.c:236`/`endpoint.c:77` cap) or a `fragment_index` out of range (the `receiver.c:281` NULL guard). Both paths require a malformed-input test the suite lacks.
- **Med — `delivery-test-single.c` lacks a post-`uv_run` completion assert.** It relies on hang-until-timeout instead of asserting `received == total` like `delivery-test-multiple.c:333`.
- **Med — No single-fragment, out-of-order, or duplicate-fragment coverage.** Both files only send multi-fragment payloads (3200→3 frags, 1500→2 frags) and the in-memory transport delivers fragments **synchronously in order**, so the receiver's reorder buffer and dedup (`receiver.c:283`) are never exercised. Drop/retransmit is covered **only** in the single file (`:153`, every 5th packet dropped).
- **Low — `delivery-test-multiple.c` nits:** `(void)parcel` then uses `parcel` (`:83-87`); `read` return ignored in the byte-compare loop (`:89-93`); missing `\n` (`:68`).

Good: both files **byte-verify** the reassembled payload against the sent data (not vacuous); `delivery-test-multiple.c` has a correct post-run completion gate and fan-out (1→3) coverage.

---

## API & Plugin — `api-basic-test.c`, `api-broadcast-test.c`, `plugin-test.c`, `webrtc-plugin-test.c`

- **High — Only RELIABLE mode is roundtrip-tested.** Both API tests set RELIABLE on both ends (`api-basic-test.c:279/313`); SEQUENCED and UNRELIABLE send/recv are never exercised anywhere in the public-API suite.
- **High — Invalid-channel hardening fix uncovered (T2).** No test sends on an out-of-range channel index, so `session.c:62`'s new `dispatch_send_result`-on-invalid-channel path has zero regression coverage.
- **High — `webrtc-plugin-test.c` is dead/unrun.** Built but never `add_test`'d; requires an external plugin path as `argv[1]`; uses `assert()` (compiled out under `-DNDEBUG`) and `printf`-only verification (`:141-181`). Contributes no coverage. Decide: wire it into CTest with a fixture, or drop it.
- **🐞 Med — `plugin-test.c` receive assertion is dead.** `pomelo_socket_on_received` asserts `value == 4455` (`:169-183`) but its only trigger `plugin->session_receive(...)` is **commented out** (`:363`, "TODO: Update this test"). Multiple "TODO" markers (`:254`, `:360`) indicate the plugin test is half-finished. Combined with T1 (no post-run flag), the behavioral assertions can pass vacuously.
- **Med — Broadcast assertion is weak.** `api-broadcast-test.c` uses a single global `recv_counter` and only requires `>= NCLIENTS` (`:289`) — one client receiving N copies satisfies it identically to N distinct clients. True distinct-session fan-out is not asserted.
- **Med — T1 applies:** `api-basic`/`api-broadcast` assert nothing post-`uv_run` that the exchange occurred (connect is partly guarded by an in-callback `SUCCESS` assert that may never fire).
- **Med — No disconnect / plugin-unload / error-path coverage.** `pomelo_session_disconnect`, `plugin_demo_on_unload` (empty stub `:215`), and NULL-arg/invalid-config paths are untested.
- **Low — Stale comment** `#define ..._TIMEOUT -1 // 1 second` in both API tests (value is -1 = no timeout).

Good: `api-basic-test.c` is a genuine bidirectional roundtrip with **byte verification both directions** (`:214`, `:263`) + dual leak checks; `plugin-test.c` verifies the loader (`extra_data == 1234`) and channel-mode readback.

---

## Prioritized remediation backlog

> Status as of 2026-10-08: **P0, P1, P2 are DONE** (committed on `main`, local only). P3 (breadth) remains as a follow-up. The suite is now **16 CTest tests** (added `pomelo-test-protocol-server-dos` and `pomelo-test-delivery-malformed`), all green.

**P0 — test-correctness bugs (the test is wrong): ✅ DONE**
1. ✅ `crypto-test.c:198/208` — now captures & checks the decrypt return.
2. ✅ `delivery-test-single.c:141` — duplicate `total_transmission_count +=` removed.
3. ✅ `pool-test.c:73` — asserts `*data2 == 2`.
4. ✅ `platform-test.c` — timer interval now validated in ns (sticky-fail on bad interval); duplicate assert dropped.
5. ✅ `plugin-test.c` — dead `4455` assertion removed, stale TODOs resolved (receive path left honestly uncovered; see P3).

**P1 — close the vacuous-pass hole (T1): ✅ DONE** — post-`uv_run` completion assertions added to `protocol-test.c` (`received_count`), `client-test.c`/`server-test.c` (`*_completed` + connect-result SUCCESS), `api-basic-test.c` (`exchange_done`), `api-broadcast-test.c` (distinct-session fan-out). The `protocol-test.c` backstop exposed a **real harness bug** (server stopped the client before the echo was delivered → client checks never ran); fixed by moving teardown into the client's receive handler after the echo is verified. *(Watchdog-timeout for a true stuck-handle hang was deferred — the completion asserts already convert a drained-but-incomplete run into a failure.)*

**P2 — regression tests for the uncovered hardening fixes (T2): ✅ DONE**
- ✅ Crypto negative path — AEAD tamper (ciphertext/AAD/wrong-key) + `decode_private` wrong-key/tamper, all fail-closed (`crypto-test.c`).
- ✅ Body-length underflow — `validate_body_length` contract across all encrypted types + truncated body through the real decode path (`packet-test.c`). *(Guard is defense-in-depth; not isolatable by revert without sanitizers.)*
- ✅ Half-open peer cap — new `pomelo-test-protocol-server-dos`; flood 16 REQUESTs from distinct addresses vs `max_clients=4`, peak peers never exceeds the cap. **Revert-proof isolated** (peak → 16 without the guard).
- ✅ Delivery `last_index` cap + NULL-fragment — new `pomelo-test-delivery-malformed`. **Both guards revert-proof isolated** (removing `receiver.c:281` segfaults, proving the guard prevents a real crash).
- ✅ Invalid-channel send result — out-of-range send still fires `on_send_result` with `send_count==0` (`api-basic-test.c`). Revert-proof isolated against `session.c:62`.
- ✅ Executor cross-thread — foreign-thread submit with a distinct sentinel; cross-thread `uv_async` + submit/shutdown mutex exercised (`platform-test.c`). Deterministic, non-flaky.

**P3 — coverage breadth (follow-up, not yet done):** SEQUENCED/UNRELIABLE roundtrips; replay duplicate-sequence; protocol rejection paths (bad/expired/wrong-key token, version/protocol-id mismatch, wrong-state packets); map collision/rehash; ref finalized-revival (needs NDEBUG); checksum/base64/challenge-token codec; single-fragment/out-of-order/duplicate delivery; plugin receive path (complete the session exchange); `webrtc-plugin-test` into CTest or removed; a watchdog-timeout for the integration drivers.
