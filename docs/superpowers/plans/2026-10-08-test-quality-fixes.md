# Test Quality Fixes Implementation Plan (P0→P2)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Fix the 5 verified test-correctness bugs (P0), close the systemic vacuous-pass/hang hole in the integration drivers (P1), and add regression tests for the 6 uncovered hardening fixes plus the core crypto negative path (P2) — as catalogued in [`docs/TEST_REVIEW.md`](../../TEST_REVIEW.md).

**Architecture:** All work is in `test/`. P0/P1 edit existing files in place; P2 adds new `test_*` functions (unit files) or new scripted scenarios/targets (integration). No production `src/` code changes — the hardening fixes already exist on `main`; P2 proves they work and guards against regression. Where a P2 test targets a specific guard, the task includes a **"revert-proof" check**: temporarily comment the guard, confirm the new test fails, then restore the guard. This proves the test actually exercises the fix.

**Tech Stack:** C11, CMake + CTest, `test/pomelo-test.h` harness (`pomelo_check`, `pomelo_run_test`), libuv loop, libsodium.

**Build/run a single test target:**
```bash
cmake --build build --target <target> && ctest --test-dir build -R <ctest-name> --output-on-failure
```
Target↔CTest-name are identical (e.g. `pomelo-test-crypto`). Full suite: `ctest --test-dir build --output-on-failure`.

**Global conventions:**
- Completion flags are `static volatile int` at file scope, set inside the callback that proves the behavior, asserted **after** `uv_run`/`pomelo_test_platform_run` returns (model: `delivery-test-multiple.c:333`).
- Every task ends by rebuilding its target and running it green, then a commit. Commit messages end with the footer:
  ```
  Co-Authored-By: claude-flow <ruv@ruv.net>
  ```
- Do not push. Commit locally only.

---

## P0 — Test-correctness bugs (the test itself is wrong)

### Task 1: Fix vacuous decrypt check in crypto-test

**Files:**
- Modify: `test/crypto-test/crypto-test.c:197-210`

The decrypt call's return is discarded; the following `pomelo_check(ret == 0)` re-checks the stale `ret` from the *encrypt* call. Capture the decrypt return. Also factor the trailing AEAD block out of `main()` into a named test for consistency (optional but preferred).

- [ ] **Step 1: Capture the decrypt return.** In `test/crypto-test/crypto-test.c`, change the decrypt call (currently `pomelo_crypto_decrypt_aead(...)` with no assignment at line ~198) to assign `ret`:

```c
    printf("Decrypting message\n");
    ret = pomelo_crypto_decrypt_aead(
        (uint8_t *) decrypted_msg,
        &output_length,
        (uint8_t *) encrypted_msg,
        sizeof(encrypted_msg),
        key,
        nonce,
        ad,
        sizeof(ad)
    );
    pomelo_check(ret == 0);
    pomelo_check(output_length == sizeof(raw_msg));
    pomelo_check(memcmp(raw_msg, decrypted_msg, sizeof(raw_msg)) == 0);
```

- [ ] **Step 2: Build and run.**

Run: `cmake --build build --target pomelo-test-crypto && ctest --test-dir build -R pomelo-test-crypto --output-on-failure`
Expected: PASS.

- [ ] **Step 3: Commit.**

```bash
git add test/crypto-test/crypto-test.c
git commit -m "test(crypto): check decrypt_aead return instead of stale encrypt ret"
```

---

### Task 2: Remove double-count in delivery-test-single

**Files:**
- Modify: `test/delivery-test/delivery-test-single.c:125,141`

`total_transmission_count += transmission_count;` appears twice in `pomelo_delivery_sender_on_result` (line 125 and line 141). Delete the second occurrence (the one just before `check_finish()`), keeping the first (before the reader loop).

- [ ] **Step 1: Delete the duplicate.** Remove the second `total_transmission_count += transmission_count;` (the line immediately after `pomelo_delivery_parcel_unref(parcel);` near line 141). The first occurrence at line ~125 stays.

- [ ] **Step 2: Build and run.**

Run: `cmake --build build --target pomelo-test-delivery-single && ctest --test-dir build -R pomelo-test-delivery-single --output-on-failure`
Expected: PASS (counter now correct; completion gate unaffected).

- [ ] **Step 3: Commit.**

```bash
git add test/delivery-test/delivery-test-single.c
git commit -m "test(delivery): remove double-count of transmission_count"
```

---

### Task 3: Fix wrong-variable assertion in pool-test

**Files:**
- Modify: `test/utils-test/pool-test.c:73`

After acquiring `data2` (line 71), the test asserts `*data == 2` — the stale first pointer. It should assert the freshly-acquired element.

- [ ] **Step 1: Assert the right variable.** Change `pomelo_check(*data == 2);` at line 73 to `pomelo_check(*data2 == 2);`

- [ ] **Step 2: Build and run.**

Run: `cmake --build build --target pomelo-test-utils && ctest --test-dir build -R pomelo-test-utils --output-on-failure`
Expected: PASS (confirms the second-acquire init path writes 2 into the new element).

- [ ] **Step 3: Commit.**

```bash
git add test/utils-test/pool-test.c
git commit -m "test(utils): assert freshly-acquired pool element (data2 not data)"
```

---

### Task 4: Fix timer-interval unit mismatch + duplicate assert in platform-test

**Files:**
- Modify: `test/platform-test/platform-test.c:137-151,438`

`delta = pomelo_platform_hrtime() - last` is nanoseconds (`time.c:8` → `uv_hrtime()`), compared against `TEST_TIMER_INTERVAL == 100` (milliseconds). `delta < 100` is never true → the validation branch is dead. Convert the threshold to ns and make the bad branch record a sticky failure instead of a silent `return`. Allow slack for scheduler jitter (timers may fire slightly early): require `delta` to be at least ~80% of the interval.

- [ ] **Step 1: Fix the comparison and fail-flag.** Replace the body of `pomelo_test_timer_callback` (lines 137-150) with:

```c
    uint64_t time = pomelo_platform_hrtime(platform);
    uint64_t delta = time - test_timer_last_callback_time;
    // hrtime() is nanoseconds; TEST_TIMER_INTERVAL is milliseconds.
    // Allow 20% slack for scheduler jitter / early wakeups.
    uint64_t min_delta_ns =
        (uint64_t) TEST_TIMER_INTERVAL * 1000000ULL * 8 / 10;
    if (delta < min_delta_ns) {
        printf("[!] Timer fired too early: %llu ns\n",
            (unsigned long long) delta);
        test_timer_result = -1; // sticky failure
        return;
    }
    test_timer_last_callback_time = time;

    if (*counter == 5) {
        pomelo_platform_timer_stop(platform, &timer);
        if (test_timer_result != -1) {
            test_timer_result = 1;
        }
        platform_done();
    }
```

Note: the first callback compares against the time captured in `pomelo_test_platform_timer` (line 158), so the first delta is also a real interval. If the harness proves flaky on the very first tick, skip the check when `*counter == 1`.

- [ ] **Step 2: Remove the duplicate assert.** In `main` (the leak/result block near lines 437-438) delete the second `pomelo_check(test_timer_result == 1);` so it appears once. Keep the final assert as `pomelo_check(test_timer_result == 1);` (so a sticky `-1` fails the test).

- [ ] **Step 3: Build and run.**

Run: `cmake --build build --target pomelo-test-platform-uv && ctest --test-dir build -R pomelo-test-platform-uv --output-on-failure`
Expected: PASS (interval now genuinely validated).

- [ ] **Step 4: Revert-proof check.** Temporarily set `min_delta_ns` absurdly high (e.g. `delta < UINT64_MAX`), rebuild+run → the test MUST now fail (`test_timer_result == -1`). Restore the correct expression. This confirms the branch is live.

- [ ] **Step 5: Commit.**

```bash
git add test/platform-test/platform-test.c
git commit -m "test(platform): validate timer interval in ns, drop duplicate assert"
```

---

### Task 5: Resolve dead receive assertion + TODOs in plugin-test

**Files:**
- Modify: `test/plugin-test/plugin-test.c`

`pomelo_socket_on_received` asserts `value == 4455` (lines 169-183) but its only trigger `plugin->session_receive(...)` is commented out (line 363), and the test never establishes a real connection (the server socket is created but nothing connects), so neither `on_connected` nor `on_received` fire. The test currently only verifies the loader (`extra_data == 1234`). Make the test honest about what it covers.

Scope decision (pick the minimal honest fix): since wiring a full plugin client↔server session exchange is a larger effort, this task makes the *existing* assertions non-dead and adds a fired-flag for what is genuinely exercised, and leaves a precise TODO for the unimplemented exchange.

- [ ] **Step 1: Add a loader-fired flag and keep it meaningful.** The loader path IS exercised (`extra_data == 1234` at line 85). Leave that assert. Add at file scope:

```c
static volatile int plugin_send_fired = 0;
```

- [ ] **Step 2: Make `plugin_demo_session_send` set the flag.** In `plugin_demo_session_send` (around lines 342-364), after the existing byte checks (`read_int32 == 1234`, `channel_index == 0`), add `plugin_send_fired = 1;` and REMOVE the commented-out/dead `// plugin->session_receive(plugin, session, 1, NULL);` line (363) along with the stale `// TODO: Update this test` if the exchange is not being implemented here. If `plugin_demo_session_send` is only reached via a real `pomelo_session_send`, confirm whether the current scripted flow reaches it; if it does not (no connection), do NOT assert `plugin_send_fired` in main — instead delete the now-unreachable `pomelo_socket_on_received` `value == 4455` assertion body and replace with a `(void)` guard + a comment: `// Receive path not yet exercised by this test (no session established).`

- [ ] **Step 3: Remove/neutralize the dead 4455 assertion.** In `pomelo_socket_on_received` (lines 169-183), since nothing triggers it, replace the `value == 4455` assertion with a byte-verified assertion against whatever the test actually sends IF a real exchange is wired; otherwise keep the handler but drop the unreachable magic-number check so it cannot masquerade as coverage. Leave a one-line comment pointing at `docs/TEST_REVIEW.md` "plugin-test" for the follow-up.

- [ ] **Step 4: Build and run.**

Run: `cmake --build build --target pomelo-test-plugin && ctest --test-dir build -R pomelo-test-plugin --output-on-failure`
Expected: PASS.

- [ ] **Step 5: Commit.**

```bash
git add test/plugin-test/plugin-test.c
git commit -m "test(plugin): remove dead receive assertion, resolve stale TODOs"
```

> Note for executor: plugin-test is genuinely half-finished. If wiring a real plugin session exchange is in scope for this pass, prefer completing it (connect a client, send, assert a byte-verified value + fired flag) over neutralizing the assertion. Decide based on effort; either way the end state must contain no dead assertion that pretends to verify the receive path.

---

## P1 — Close the vacuous-pass / hang hole (completion backstops)

Pattern for each file: add a `static volatile int` completion counter/flag, set it in the proving callback, and assert it **after** the loop returns, **before** the existing leak check. This turns a silent no-op into a hard failure.

### Task 6: Backstop payload receive in protocol-test

**Files:**
- Modify: `test/protocol-test/protocol-test.c:228,319-338`

Handshake is already gated (`connected_count == 2` at line 228); the payload echo is not. Add a received counter.

- [ ] **Step 1: Add the counter.** Near the other file-scope counters (where `connected_count` is declared), add:

```c
static volatile int received_count = 0;
```

- [ ] **Step 2: Increment it in the client receive branch.** In `pomelo_protocol_socket_on_received`, inside the `else` (client) branch, after the `pomelo_check(v2 == sample_v2);` at line 335 (before `pomelo_protocol_peer_disconnect`), add:

```c
        received_count++;
```

- [ ] **Step 3: Assert after the run.** In `main`, right after `pomelo_check(connected_count == 2);` (line 228) add:

```c
    pomelo_check(received_count == 1);
```

- [ ] **Step 4: Build and run BOTH protocol-test targets.**

Run: `cmake --build build --target pomelo-test-protocol pomelo-test-protocol-unencrypted && ctest --test-dir build -R 'pomelo-test-protocol$|pomelo-test-protocol-unencrypted' --output-on-failure`
Expected: PASS for both.

- [ ] **Step 5: Commit.**

```bash
git add test/protocol-test/protocol-test.c
git commit -m "test(protocol): assert payload received after run (anti vacuous-pass)"
```

---

### Task 7: Backstop handshake completion in protocol client-test & server-test

**Files:**
- Modify: `test/protocol-test/client-test.c` (main tail at lines 607-609; `on_connected` ~366, `on_received` ~415, `on_connect_result` ~449)
- Modify: `test/protocol-test/server-test.c` (main tail at lines 575-577; `on_connected` ~323, `on_received` ~371, `on_connect_result` ~403)

Both pass vacuously: `main()` asserts only the leak check. Add a completion flag set in `on_received` (the deepest proof the handshake + payload exchange ran) and assert it before the leak check. Also make `on_connect_result` assert success for the connecting side.

- [ ] **Step 1 (client-test.c): add flag.** At file scope add `static volatile int client_completed = 0;`

- [ ] **Step 2 (client-test.c): set it.** In the client's `on_received` handler, after the existing value checks (lines ~435/439), add `client_completed = 1;`

- [ ] **Step 3 (client-test.c): assert success in connect-result.** In `on_connect_result` (line ~449), replace the bare `printf` with:

```c
    pomelo_check(result == POMELO_PROTOCOL_SOCKET_CONNECT_SUCCESS);
```
(keep any printf if useful, but the assert must be present).

- [ ] **Step 4 (client-test.c): assert after run.** In `main`, immediately before `pomelo_check(alloc_bytes == ...)` (line ~607), add `pomelo_check(client_completed == 1);`

- [ ] **Step 5 (server-test.c): repeat.** Add `static volatile int server_completed = 0;`; set `server_completed = 1;` in the server's `on_received` after the `v1==sample_v1` checks (lines ~391/395); add `pomelo_check(server_completed == 1);` before the leak check (line ~575). For `on_connect_result` (line ~403): the server is not the connecting side, so assert only if the handler is expected to fire with success in this harness — otherwise leave as-is.

- [ ] **Step 6: Build and run both.**

Run: `cmake --build build --target pomelo-test-protocol-client pomelo-test-protocol-server && ctest --test-dir build -R 'pomelo-test-protocol-client|pomelo-test-protocol-server' --output-on-failure`
Expected: PASS for both. (If either now fails, the handshake was silently broken — investigate before proceeding; that is the bug this task is meant to surface.)

- [ ] **Step 7: Commit.**

```bash
git add test/protocol-test/client-test.c test/protocol-test/server-test.c
git commit -m "test(protocol): assert handshake completion in client/server tests"
```

---

### Task 8: Backstop + watchdog in api-basic & api-broadcast

**Files:**
- Modify: `test/api-test/api-basic-test.c` (main tail 166-168; `check_finished`, `on_received` ~218)
- Modify: `test/api-test/api-broadcast-test.c` (main tail 168-170; `check_finished` ~289; `recv_counter` ~363; per-client tracking)

- [ ] **Step 1 (api-basic): add flag.** At file scope add `static volatile int exchange_done = 0;`. Set `exchange_done = 1;` inside `check_finished()` (the function the client calls at line 218 after verifying `value == 24`). In `main`, before `pomelo_check(alloc_bytes == ...)` (line 167), add `pomelo_check(exchange_done == 1);`

- [ ] **Step 2 (api-broadcast): assert fan-out to DISTINCT sessions.** Replace the single global `recv_counter >= NCLIENTS` logic with distinct-session tracking. At file scope add:

```c
static volatile int client_received[API_TEST_NCLIENTS] = {0};
static volatile int distinct_received = 0;
```
In `pomelo_client_on_received`, map the receiving `client`/`session` to its index `i` (the test already creates `clients[i]`; find the index by comparing `client == clients[i]`), and do:

```c
    if (!client_received[idx]) {
        client_received[idx] = 1;
        distinct_received++;
    }
```
In `main`, before the leak check (line 169), add `pomelo_check(distinct_received == API_TEST_NCLIENTS);` and `pomelo_check(connected_counter == API_TEST_NCLIENTS);`

- [ ] **Step 3 (both): add a watchdog timeout.** `pomelo_test_platform_run` has no timeout, so a stall hangs. Add a one-shot platform timer (e.g. 5000 ms) started before the run that, on fire, sets a `static volatile int timed_out = 1;` and stops the sockets so `uv_run` returns. In `main` after the run assert `pomelo_check(timed_out == 0);`. Use `pomelo_platform_timer_start` with interval 0 (one-shot) — see `platform-test.c:160` for the call shape. If adding a watchdog proves too invasive for this pass, document it as a follow-up and rely on the completion asserts (which still convert a *drained-but-incomplete* run into a failure; only a truly stuck handle still hangs).

- [ ] **Step 4: Build and run.**

Run: `cmake --build build --target pomelo-test-api-basic pomelo-test-api-broadcast && ctest --test-dir build -R 'pomelo-test-api-basic|pomelo-test-api-broadcast' --output-on-failure`
Expected: PASS for both.

- [ ] **Step 5: Commit.**

```bash
git add test/api-test/api-basic-test.c test/api-test/api-broadcast-test.c
git commit -m "test(api): assert exchange completion + distinct-session broadcast"
```

---

## P2 — Regression tests for uncovered hardening fixes + crypto negative path

Each P2 task includes a **revert-proof check**: comment out the guard in `src/`, confirm the new test fails, restore the guard. Do NOT commit the reverted guard.

### Task 9: Crypto negative path (fail-closed) + decode_private tamper

**Files:**
- Modify: `test/crypto-test/crypto-test.c` (add tests + register in `main`)

- [ ] **Step 1: Add a tampered-AEAD rejection test.** Add a new function that encrypts `raw_msg`, flips one ciphertext byte, and asserts decrypt returns `< 0`; repeat flipping one AAD byte; repeat with a wrong key:

```c
static int test_aead_rejects_tampered(void) {
    pomelo_track_function();
    uint8_t k[32], n[12], a[16];
    pomelo_random_buffer(k, sizeof(k));
    pomelo_random_buffer(a, sizeof(a));
    pomelo_crypto_make_nonce(n, sizeof(n), 42);

    uint8_t ct[sizeof(raw_msg) + POMELO_CRYPTO_AEAD_HMAC_BYTES];
    size_t ct_len = 0;
    pomelo_check(pomelo_crypto_encrypt_aead(ct, &ct_len,
        (uint8_t *) raw_msg, sizeof(raw_msg), k, n, a, sizeof(a)) == 0);

    uint8_t out[sizeof(raw_msg)];
    size_t out_len = 0;

    // 1) tampered ciphertext
    uint8_t bad[sizeof(ct)]; memcpy(bad, ct, ct_len);
    bad[0] ^= 0x01;
    pomelo_check(pomelo_crypto_decrypt_aead(out, &out_len, bad, ct_len,
        k, n, a, sizeof(a)) < 0);

    // 2) tampered AAD
    uint8_t bad_ad[16]; memcpy(bad_ad, a, sizeof(a)); bad_ad[0] ^= 0x01;
    pomelo_check(pomelo_crypto_decrypt_aead(out, &out_len, ct, ct_len,
        k, n, bad_ad, sizeof(bad_ad)) < 0);

    // 3) wrong key
    uint8_t wrong_k[32]; pomelo_random_buffer(wrong_k, sizeof(wrong_k));
    pomelo_check(pomelo_crypto_decrypt_aead(out, &out_len, ct, ct_len,
        wrong_k, n, a, sizeof(a)) < 0);
    return 0;
}
```

- [ ] **Step 2: Add a decode_private tamper/wrong-key test.** Model on `test_token_private_roundtrip` (line 116): encode a private token, then (a) decode with a wrong server key → expect `< 0`; (b) flip a byte of `priv[]` → expect `< 0`. Assert no out field is trusted on failure.

- [ ] **Step 3: Register both in `main`.** Add `pomelo_run_test(test_aead_rejects_tampered);` and the decode_private negative test after the existing `pomelo_run_test` calls (line ~172).

- [ ] **Step 4: Build, run, revert-proof.** Run `pomelo-test-crypto`. Expected PASS. Confirm the AEAD/MAC check is real: these use libsodium directly, so instead of reverting a guard, confirm by asserting the *un*-tampered inputs still decrypt (already covered by Task 1).

- [ ] **Step 5: Commit.**

```bash
git add test/crypto-test/crypto-test.c
git commit -m "test(crypto): add AEAD tamper + decode_private wrong-key rejection"
```

---

### Task 10: Body-length underflow guard (packet.c:224)

**Files:**
- Modify: `test/protocol-test/packet-test.c` (add a decode-rejection test + register)

The guard `validate_body_length` rejects `body_length < POMELO_HMAC_BYTES`. Craft an encrypted-packet decode with a truncated body and assert it is rejected.

- [ ] **Step 1: Add the test.** Build a PAYLOAD (or any encrypted) packet buffer whose declared/available body length is below `POMELO_HMAC_BYTES`, run it through the same decode entry the existing `packet-test.c` encode/decode tests use (see the PAYLOAD roundtrip at line ~546 for the decode call shape), and assert the decode returns an error (`< 0`) rather than reading past the buffer. Use the real `pomelo_protocol_*` decode path so `validate_body_length` is on the stack.

- [ ] **Step 2: Register** the new `test_*` in `main` (lines ~756-762), following the file's existing `pomelo_check(test_xxx() == 0);` style.

- [ ] **Step 3: Build and run.** Run `pomelo-test-protocol-packet`. Expected PASS.

- [ ] **Step 4: Revert-proof.** Comment the `body_length < POMELO_HMAC_BYTES` guard at `src/protocol/packet.c:224`, rebuild the test, run under a sanitizer build if available (`-fsanitize=address`) — the new test MUST fail or trip ASan. Restore the guard.

- [ ] **Step 5: Commit.**

```bash
git add test/protocol-test/packet-test.c
git commit -m "test(protocol): regression for body-length underflow guard"
```

---

### Task 11: Half-open (requesting) peer cap (server.c:320)

**Files:**
- Modify: `test/protocol-test/server-test.c` (add a flood scenario) OR add a new focused test file if cleaner.

The cap rejects a new REQUEST once `requesting_peers->size >= max_clients`. Drive more distinct-address REQUESTs than `max_clients` without completing any handshake, and assert the server stops allocating requesting peers past the cap.

- [ ] **Step 1: Understand the harness.** `server-test.c` already feeds a hand-rolled client via `send_handler` and sends a REQUEST via `send_request_packet`. Read how it builds a REQUEST and how `max_clients`/`MAX_CLIENTS` is set (line ~532).

- [ ] **Step 2: Add the flood.** Send `MAX_CLIENTS + N` REQUESTs from distinct source addresses (vary the simulated client address) WITHOUT sending RESPONSE, so they stay half-open. Assert via the protocol statistics (`pomelo_protocol_context_statistic`) or an observable counter that the number of requesting/allocated peers never exceeds `MAX_CLIENTS`. Prefer asserting the server returns/ignores REQUESTs beyond the cap rather than growing unbounded.

- [ ] **Step 3: Register/run.** Build `pomelo-test-protocol-server`. Expected PASS. Keep the existing happy-path scenario intact (add the flood as a separate scenario or a separate test target if it conflicts with the single-`main` structure).

- [ ] **Step 4: Revert-proof.** Comment the cap at `src/protocol/server.c:320`, rebuild+run → peer count should exceed `MAX_CLIENTS` and the assertion must fail. Restore.

- [ ] **Step 5: Commit.**

```bash
git add test/protocol-test/server-test.c
git commit -m "test(protocol): regression for half-open peer cap (anti-DoS)"
```

> If weaving the flood into server-test.c's single-`main` is awkward, create `test/protocol-test/server-dos-test.c` + a `pomelo-test-protocol-server-dos` target in CMakeLists.txt (mirror the server-test block at CMakeLists.txt:470-490) + an `add_test`. Document the choice in the commit.

---

### Task 12: Delivery malformed-fragment guards (last_index cap + NULL fragment)

**Files:**
- Modify: `test/delivery-test/delivery-test-single.c` or new `test/delivery-test/delivery-malformed-test.c`

Two guards: `last_index >= max_fragments` (`bus.c:236`, `endpoint.c:77`) and out-of-range `fragment_index` → NULL fragment (`receiver.c:281`). Both operate on a crafted fragment meta arriving at the receiver.

- [ ] **Step 1: Locate the injection point.** In the delivery tests, fragments reach the receiver via `pomelo_delivery_endpoint_send` (the test double) → the bus recv path. Read `src/delivery/bus.c` `recv_fragment_data` and the fragment meta layout to construct a fragment whose packed `last_index` exceeds `context->max_fragments`, and one whose `fragment_index` is out of range.

- [ ] **Step 2: Add a test** that hands such crafted fragments to the receiver/bus recv entry and asserts it returns `-1`/ignores them WITHOUT crashing (no OOB, no abort). Run under ASan if available to prove no out-of-bounds.

- [ ] **Step 3: Build and run.** Expected PASS.

- [ ] **Step 4: Revert-proof.** Comment each guard in turn (`bus.c:236`, then `receiver.c:281`), rebuild under ASan, confirm the crafted-input test fails/trips. Restore both.

- [ ] **Step 5: Commit.**

```bash
git add test/delivery-test/*.c CMakeLists.txt
git commit -m "test(delivery): regression for last_index cap and NULL-fragment guard"
```

---

### Task 13: Invalid-channel send result (session.c:62)

**Files:**
- Modify: `test/api-test/api-basic-test.c` (add an invalid-channel send + result assertion)

The fix makes a send on an out-of-range channel still call `pomelo_socket_dispatch_send_result` (so the caller gets a result instead of a silent drop). Send on `channel_index == nchannels` (one past the end) and assert a send-result callback fires with a failure result.

- [ ] **Step 1: Add the test path.** After the normal exchange is established (inside `on_ready` or a dedicated step), call `pomelo_session_send(session, API_TEST_CHANNELS, message, info_ptr)` with an out-of-range channel index, and provide a send-result callback (or use the existing one) that sets a `static volatile int invalid_send_result_fired = 0;` and records the result code.

- [ ] **Step 2: Assert after run.** `pomelo_check(invalid_send_result_fired == 1);` and assert the result code indicates failure (not success). Read `include/pomelo/api.h` for the send-result callback signature and the result enum values.

- [ ] **Step 3: Build and run.** Expected PASS.

- [ ] **Step 4: Revert-proof.** Comment the `pomelo_socket_dispatch_send_result(...)` call at `src/api/session.c:62`, rebuild+run → the result callback never fires and the assertion fails. Restore.

- [ ] **Step 5: Commit.**

```bash
git add test/api-test/api-basic-test.c
git commit -m "test(api): regression for invalid-channel send result dispatch"
```

---

### Task 14: Executor cross-thread submit/shutdown (executor.c mutex)

**Files:**
- Modify: `test/platform-test/platform-test.c` or new `test/platform-test/executor-test.c`

The executor's purpose is cross-thread submission; the current test submits from the loop thread, so the submit-vs-shutdown mutex has no coverage. Submit from a real foreign thread.

- [ ] **Step 1: Spawn a worker thread** (uv_thread_create) that calls `pomelo_platform_uv_executor_submit` (or the public submit API) with a distinct non-NULL sentinel pointer while the loop runs. The executor callback (loop thread) asserts it received that exact sentinel and sets a `static volatile int executor_crossthread_fired = 0;`.

- [ ] **Step 2: Exercise submit-vs-shutdown.** Optionally, have the foreign thread submit repeatedly while the main thread initiates platform close, to exercise the serialization mutex (`executor.c`). At minimum, assert the single cross-thread job is delivered with the correct pointer.

- [ ] **Step 3: Assert after run.** `pomelo_check(executor_crossthread_fired == 1);` Replace the existing near-vacuous `NULL == NULL` executor assertion (`platform-test.c:375`) with the sentinel comparison.

- [ ] **Step 4: Build and run** (ideally with `-fsanitize=thread` if the build supports it). Expected PASS.

- [ ] **Step 5: Commit.**

```bash
git add test/platform-test/platform-test.c CMakeLists.txt
git commit -m "test(platform): cross-thread executor submit with sentinel + shutdown race"
```

---

## P3 breadth — batch 1

> **Status: ✅ ALL DONE (2026-10-08, committed local).** Task 15 `0a4de51`, Task 16 `0c6e875`, Task 18 `2938133`, Task 17 `0aadde5`. Suite 16 → 17 tests, all green. Implementation notes captured in `docs/TEST_REVIEW.md` (P3 batch 1). A P3 batch 2 (protocol rejection paths, delivery reorder/dedup, plugin receive path, challenge-token codec, integration watchdog) remains as a follow-up.

### Task 15: SEQUENCED / UNRELIABLE API roundtrip — ✅ DONE (`0a4de51`)

**Files:**
- Modify: `test/api-test/api-basic-test.c` (parameterize channel mode) or add `api-modes-test.c`.

- [ ] **Step 1:** Run the existing bidirectional roundtrip with the channel mode set to SEQUENCED, then UNRELIABLE (currently only RELIABLE at lines 279/313). For UNRELIABLE, assert eventual receipt in a loopback/no-loss simulator (the in-process platform does not drop), byte-verifying the payload.
- [ ] **Step 2:** Build, run, commit:
```bash
git commit -m "test(api): roundtrip coverage for sequenced and unreliable modes"
```

### Task 16: Replay duplicate-sequence rejection — ✅ DONE (`0c6e875`)

**Files:**
- Modify: `test/protocol-test/server-test.c` or `client-test.c`.

- [ ] **Step 1:** After a connected session, re-deliver a previously-seen encrypted packet (same sequence) and assert the 256-entry replay window drops it (no duplicate dispatch to the app; packet ignored). Read `src/protocol/peer.c` `protect_replay`.
- [ ] **Step 2:** Build, run, commit:
```bash
git commit -m "test(protocol): regression for replay-window duplicate-sequence drop"
```

### Task 17: Wire webrtc-plugin-test into CTest or remove it — ✅ DONE (`0aadde5`, wired as smoke test)

**Files:**
- Modify: `CMakeLists.txt` (and/or `test/webrtc-plugin-test/webrtc-plugin-test.c`).

- [ ] **Step 1:** Decide: if the webrtc plugin artifact can be produced in the build, add an `add_test` with the plugin path as a fixture arg and replace `assert()` with `pomelo_check`. Otherwise remove the dead target to stop implying coverage. Document the decision in the commit.
- [ ] **Step 2:** Commit:
```bash
git commit -m "test(webrtc): register in CTest with pomelo_check (or remove dead target)"
```

### Task 18: Unit breadth — map collision/rehash, ref finalized-revival, checksum/base64 — ✅ DONE (`2938133`)

**Files:**
- Modify: `test/utils-test/map-test.c`, `test/base-test/ref-test.c`, `test/crypto-test/crypto-test.c`.

- [ ] **Step 1 (map):** add colliding keys (same bucket) + enough inserts to force a rehash + an iterator test.
- [ ] **Step 2 (ref):** cover the CAS "refuse to revive finalized object" path — requires an NDEBUG build path or guarding the `assert(false)`; if infeasible, document why and skip.
- [ ] **Step 3 (crypto):** add BLAKE2b checksum init/update/final roundtrip (known-answer or self-consistency) and a base64 encode→decode roundtrip (including the documented no-padding edge).
- [ ] **Step 4:** Build, run, commit each area separately.

---

## P3 breadth — batch 2

> **Status: IN PROGRESS (2026-10-08).** Tasks 19–22. Each subagent implements in a private build dir and does NOT commit; the parent verifies independently (git diff + rebuild in `build` + run + revert-proof) and commits locally. Plugin receive path stays MANUAL (browser client `../pomelo-udp-webrtc`) and is intentionally out of scope for automated CTest.

### Task 19: Delivery out-of-order + duplicate fragment reassembly

**Files:**
- Create: `test/delivery-test/delivery-reorder-test.c`
- Modify: `CMakeLists.txt` (new target `pomelo-test-delivery-reorder`, mirror the delivery-single block)

On `main` the parcel-level reorder buffer lives on the dev branch; the testable reorder path here is **fragment reassembly within one parcel**. `pomelo_delivery_receiver_add_fragment` (`src/delivery/receiver.c:269`) places each fragment by `meta->fragment_index` (not arrival order) and dedups via `if (fragment->content.buffer) return;` at `receiver.c:284`.

- [ ] **Step 1:** Clone the `delivery-test-single.c` harness. Use a RELIABLE parcel whose payload is large enough to span **multiple fragments** (assert `parcel->chunks->size > 1`). Disable the random drop (no `drop_chance % 5` drop) so delivery is deterministic.
- [ ] **Step 2:** In the transporter double `pomelo_delivery_endpoint_send`, buffer each outgoing fragment packet (copy the combined view) into a queue instead of recv-ing immediately; once the parcel's fragments are all queued, flush them to the receiver in **reverse order** (out-of-order), and additionally recv **one fragment twice** (duplicate).
- [ ] **Step 3:** Assert in `pomelo_delivery_bus_on_received` that the reassembled bytes equal the sent payload exactly, and that the received callback fires **exactly once** for the parcel (a `static volatile int` counter asserted after `uv_run`). This proves out-of-order reassembly + dedup.
- [ ] **Step 4 (revert-proof):** Comment the dedup guard `receiver.c:284` (`if (fragment->content.buffer) return;`), rebuild+run → the duplicate fragment double-increments `recv_fragments`, so the parcel either never completes or over-counts → test MUST fail. Restore.
- [ ] **Step 5: Commit** `test(delivery): out-of-order + duplicate fragment reassembly`.

### Task 20: Challenge-token codec roundtrip + negative paths

**Files:**
- Modify: `test/crypto-test/crypto-test.c` (add tests + register in `main`)

`pomelo_codec_encrypt_challenge_token` / `pomelo_codec_decrypt_challenge_token` (`src/crypto/token.c:466,517`) are untested. Token struct = `{ int64_t client_id; uint8_t user_data[POMELO_USER_DATA_BYTES]; }` (`src/crypto/token.h:16`); ciphertext size `POMELO_CHALLENGE_TOKEN_BYTES`; nonce derived from `token_sequence`.

- [ ] **Step 1:** Roundtrip: encode into a payload with a random key + sequence, decode with the same key+sequence → assert `client_id` and all `user_data` bytes match.
- [ ] **Step 2:** Negatives (each must return `< 0`): (a) decrypt with a wrong key; (b) flip one ciphertext byte; (c) decrypt with a wrong `token_sequence` (wrong nonce). Follow the payload setup used by `test_token_private_roundtrip` (`crypto-test.c:118`).
- [ ] **Step 3:** Register in `main` after the existing `pomelo_run_test` calls (`crypto-test.c:385`).
- [ ] **Step 4:** Build+run `pomelo-test-crypto`. Revert-proof: the AEAD/MAC is libsodium's; prove it is live by the positive roundtrip (Step 1) passing while all Step-2 negatives reject.
- [ ] **Step 5: Commit** `test(crypto): challenge-token codec roundtrip + wrong-key/tamper/nonce rejection`.

### Task 21: Server rejects spoofed REQUEST (protocol_id mismatch)

**Files:**
- Create: `test/protocol-test/server-reject-test.c` (clone the `server-dos-test.c` harness)
- Modify: `CMakeLists.txt` (new target `pomelo-test-protocol-server-reject`, mirror the server-dos block)

`pomelo_protocol_server_validate` (`src/protocol/server.c:308`) rejects a REQUEST whose `protocol_id != server->protocol_id` **synchronously, before allocating any half-open peer** (`:324`). The dos harness already measures the peak `requesting_peers` via statistics.

- [ ] **Step 1:** Clone `server-dos-test.c`. Build the REQUEST exactly as `send_request_packet_from` does, but set the packet's `protocol_id` to a value **different** from the server's (e.g. `token.protocol_id ^ 0xFFFF`).
- [ ] **Step 2:** Send `FLOOD_COUNT` such wrong-protocol REQUESTs from distinct source addresses, then capture the peak synchronously (same point the dos test does). Assert the peak requesting/half-open peer count is **0** — every spoofed REQUEST was dropped at the protocol_id check.
- [ ] **Step 3:** Build+run `pomelo-test-protocol-server-reject`.
- [ ] **Step 4 (revert-proof):** Comment the `protocol_id != server->protocol_id` condition at `server.c:308` (leave only the `ret < 0` check), rebuild+run → wrong-protocol REQUESTs now allocate peers → peak > 0 → assert MUST fail. Restore.
- [ ] **Step 5: Commit** `test(protocol): server drops spoofed REQUEST with wrong protocol_id`.

### Task 22: Watchdog-timeout for integration drivers (hang → failure)

**Files:**
- Modify: `test/api-test/api-basic-test.c`, `test/api-test/api-broadcast-test.c`, `test/protocol-test/protocol-test.c`

`pomelo_test_platform_run` / `uv_run` have no timeout, so a stalled handshake hangs CTest instead of failing. Add a one-shot watchdog.

- [ ] **Step 1:** In each driver add `static volatile int timed_out = 0;` and a one-shot platform timer (interval ~5000 ms, see `platform-test.c:160` for the call shape) armed before the run. On fire, set `timed_out = 1;` and stop the socket(s) so the loop drains.
- [ ] **Step 2:** After the run, assert `pomelo_check(timed_out == 0);` (before the leak check). On the happy path the exchange completes and the watchdog is stopped/never fires; a real stall now fails fast instead of hanging.
- [ ] **Step 3:** Ensure the watchdog timer is stopped/closed on the normal completion path so it does not leak or keep the loop alive. Rebuild+run all three targets green; confirm no added leak (existing leak checks still pass).
- [ ] **Step 4: Commit** `test(integration): add watchdog timeout so a stalled run fails instead of hanging`.

---

## Final verification

- [ ] **Full suite green.** `cmake --build build -j && ctest --test-dir build --output-on-failure` → all 14 (plus any new) tests pass.
- [ ] **No reverted guard left in `src/`.** `git status` + `git diff src/` must be empty.
- [ ] **Update [`docs/TEST_REVIEW.md`](../../TEST_REVIEW.md)** remediation backlog: check off the completed P0/P1/P2 items.
- [ ] **No push** (local commits only).

## Self-review notes (coverage of the spec)

- P0 bugs 1-5 → Tasks 1-5. ✅
- T1 vacuous-pass (6 drivers) → Tasks 6-8 (protocol-test, client/server-test, api-basic/broadcast; plugin-test handled in Task 5). ✅
- T2 hardening regressions → crypto negative (9), body-length (10), half-open cap (11), delivery guards (12), invalid-channel (13), executor cross-thread (14). ✅
- Breadth → Tasks 15-18 (optional). ✅

**Risk/ambiguity notes:** Tasks 11, 12, 14 require reading harness internals to craft inputs and may need a new test file + CMakeLists target rather than editing a single-`main` driver — each task says so. Task 5 (plugin) has a scope fork documented inline. Tasks 15-18 are breadth and may be deferred to a follow-up pass without leaving any shipped fix uncovered (the shipped-fix regressions are all in Tasks 9-14).
