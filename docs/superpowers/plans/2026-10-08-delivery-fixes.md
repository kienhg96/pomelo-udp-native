# Delivery Layer Fixes Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Fix the two concrete Delivery-layer bugs from `REVIEW.md` §4 — a confirmed out-of-bounds `fragment_index` crash, and a memory-amplification via an unbounded `last_index`. (The head-of-line-blocking item is a design matter handled on the `dev/reliable-parallel` branch and is NOT in scope here.)

**Architecture:** Pure C. Two small, localized changes in `src/delivery/`. Both follow bounds-checking patterns already present elsewhere in the same layer (the send-side ACK handler already guards the exact same `array_get_ptr` call).

**Tech Stack:** C11, CMake + CTest.

---

## Ground rules for the implementer (READ FIRST)

1. **Use the `Write` tool (full-file rewrite), NOT `Edit`.** A linter/hook in this repo reverts partial edits. Read the whole file, apply the change, write it back in full.
2. **Do NOT `git push`.** Local commits only.
3. Every commit message MUST end with exactly:
   ```
   Co-Authored-By: claude-flow <ruv@ruv.net>
   ```
4. Branch off `main` first:
   ```bash
   git checkout main
   git checkout -b fix/delivery-hardening
   ```
   (Untracked files like REVIEW.md and docs/superpowers/ carry over — leave them, do not commit them.)
5. Build + test commands (build dir already configured at `build/`):
   ```bash
   cmake --build build --target pomelo-test-delivery-single pomelo-test-delivery-multiple -j
   ctest --test-dir build -R "pomelo-test-delivery" --output-on-failure
   ```
   The target `pomelo-test-general` is known-broken (missing source in a pre-existing modified `CMakeLists.txt`) — ignore it.
6. One commit per task. Run the delivery tests after every task.
7. **No failing-first test for these tasks.** Both bugs are triggered only by a maliciously crafted fragment (out-of-range `fragment_index`, or an oversized `last_index`) from a connected peer; the delivery test harness only exercises well-formed fragments, so these changes preserve all existing behavior. Verify by: the delivery test suite stays green (no regression) and re-read the diff. This is a deliberate, documented exception to TDD — do not fabricate a flaky test.

---

## Task 1: Guard out-of-range `fragment_index` on receive (🟠 High — remote crash)

**Problem:** `pomelo_delivery_receiver_add_fragment` indexes the receiver's `fragments` array with the wire-supplied `meta->fragment_index` and then `assert`s the result is non-NULL before dereferencing it. The decode path (`fragment_meta_decode`) never checks `fragment_index <= last_index`, and the array is only sized to `last_index + 1`. A connected peer can send a DATA fragment whose `fragment_index` exceeds `last_index` (e.g. `last_index = 1`, `fragment_index = 1000`); `pomelo_array_get_ptr` returns NULL, and the code dereferences it → crash (release) / assert-abort (debug). This is post-auth (fragments ride on encrypted+authenticated PAYLOAD packets), so any one connected client can crash the server with a single packet.

The send-side ACK handler `pomelo_delivery_dispatcher_recv_ack` already guards the identical call with `if (!fragment) return;` — the receiver must do the same.

**Files:**
- Modify: `src/delivery/receiver.c` (`pomelo_delivery_receiver_add_fragment`, ~lines 269-293)

- [ ] **Step 1: Read `src/delivery/receiver.c` fully.** Confirm `pomelo_delivery_receiver_add_fragment` contains:

```c
    pomelo_array_t * fragments = receiver->fragments;
    pomelo_delivery_fragment_t * fragment =
        pomelo_array_get_ptr(fragments, meta->fragment_index);
    assert(fragment != NULL);
    if (fragment->content.buffer) return; // Received
```

- [ ] **Step 2: Replace the assert with a bounds guard**

Change those lines to (drop the fragment instead of crashing when the index is out of range):

```c
    pomelo_array_t * fragments = receiver->fragments;
    pomelo_delivery_fragment_t * fragment =
        pomelo_array_get_ptr(fragments, meta->fragment_index);
    if (!fragment) {
        return; // fragment_index out of range; discard malformed fragment
    }
    if (fragment->content.buffer) return; // Received
```

(Rewrite the whole file with `Write`, preserving everything else. `array_get_ptr` returns NULL exactly when `fragment_index >= fragments->size`, so this fully prevents the out-of-bounds access for both the first and subsequent fragments.)

- [ ] **Step 3: Build & run delivery tests — verify no regression**

Run:
```bash
cmake --build build --target pomelo-test-delivery-single pomelo-test-delivery-multiple -j
ctest --test-dir build -R "pomelo-test-delivery" --output-on-failure
```
Expected: PASS — well-formed fragments always have `fragment_index <= last_index`, so the new guard never triggers for valid traffic.

- [ ] **Step 4: Commit**

```bash
git add src/delivery/receiver.c
git commit -m "fix(delivery): guard out-of-range fragment_index on receive

add_fragment indexed the fragments array with the wire-supplied
fragment_index and asserted non-NULL before dereferencing, so a crafted
fragment with fragment_index > last_index caused a NULL deref / crash
(post-auth, one packet). Drop the fragment instead, matching the
existing guard in dispatcher recv_ack.

Co-Authored-By: claude-flow <ruv@ruv.net>"
```

---

## Task 2: Cap `last_index` on receive (🟡 Medium — memory amplification)

**Problem:** `pomelo_delivery_bus_recv_fragment_data` → `ensure_recv_command` sizes a new receiver's `fragments` array to `meta->last_index + 1` with no upper bound. `last_index` is wire-supplied (up to 65535). A connected peer can send fragments with a large `last_index` across many sequences, each allocating up to 65536 fragment slots → memory exhaustion. The delivery context already carries the legitimate bound `max_fragments` (default `POMELO_DELIVERY_PARCEL_MAX_FRAGMENTS_DEFAULT`, 222).

**Files:**
- Modify: `src/delivery/bus.c` (`pomelo_delivery_bus_recv_fragment_data`, ~line 224)

- [ ] **Step 1: Read `src/delivery/bus.c`.** Confirm `pomelo_delivery_bus_recv_fragment_data` begins:

```c
int pomelo_delivery_bus_recv_fragment_data(
    pomelo_delivery_bus_t * bus,
    pomelo_delivery_fragment_meta_t * meta,
    pomelo_buffer_view_t * content
) {
    assert(bus != NULL);
    assert(meta != NULL);

    // Validate meta
    if (meta->type == POMELO_FRAGMENT_TYPE_DATA_RELIABLE) {
```

- [ ] **Step 2: Add the `last_index` bound check**

Insert the cap right after the asserts, before the `// Validate meta` block (so it guards every mode and runs before any receiver/array is allocated):

```c
int pomelo_delivery_bus_recv_fragment_data(
    pomelo_delivery_bus_t * bus,
    pomelo_delivery_fragment_meta_t * meta,
    pomelo_buffer_view_t * content
) {
    assert(bus != NULL);
    assert(meta != NULL);

    // Reject a parcel claiming more fragments than this endpoint allows.
    // last_index is 0-based, so the fragment count is last_index + 1; cap it
    // against the configured maximum to prevent memory-amplification from a
    // crafted oversized last_index.
    if (meta->last_index >= bus->context->max_fragments) {
        return -1; // Too many fragments; discard
    }

    // Validate meta
    if (meta->type == POMELO_FRAGMENT_TYPE_DATA_RELIABLE) {
```

(Rewrite the whole file with `Write`, preserving everything else. `bus->context->max_fragments` is already used elsewhere in this file, e.g. `bus->context->receiver_pool`.)

- [ ] **Step 3: Build & run delivery tests — verify no regression**

Run:
```bash
cmake --build build --target pomelo-test-delivery-single pomelo-test-delivery-multiple -j
ctest --test-dir build -R "pomelo-test-delivery" --output-on-failure
```
Expected: PASS — legitimate parcels never exceed `max_fragments` fragments, so `last_index < max_fragments` always holds for valid traffic.

- [ ] **Step 4: Commit**

```bash
git add src/delivery/bus.c
git commit -m "fix(delivery): cap received last_index to max_fragments

ensure_recv_command sized a receiver's fragments array to last_index+1
with no bound, so a crafted oversized last_index (up to 65535) forced a
huge allocation per fragment (memory amplification). Reject fragments
whose last_index >= context max_fragments before allocating.

Co-Authored-By: claude-flow <ruv@ruv.net>"
```

---

## Final verification

- [ ] Run the delivery suite once more:
```bash
ctest --test-dir build -R "pomelo-test-delivery" --output-on-failure
```
- [ ] `git log --oneline` should show 2 new commits on `fix/delivery-hardening`. Do NOT push.

---

## Out of scope (follow-up)

- 🟡 Head-of-line blocking (`bus.c` single `incomplete_reliable_dispatcher`) is a design limitation addressed by the parallel reliable mode on `dev/reliable-parallel`; not patched here.

---

## Self-Review (completed by plan author)

- **Spec coverage:** Task 1 = 🟠 OOB fragment_index crash; Task 2 = 🟡 last_index memory amplification. HOL blocking explicitly deferred.
- **Placeholder scan:** none — exact code and commands throughout.
- **Type consistency:** `meta->last_index` and `meta->fragment_index` are `size_t`; `bus->context->max_fragments` is `size_t` (context.h) — comparison is well-typed. `array_get_ptr` returning NULL for an out-of-range index is confirmed (`src/utils/array.c`: `if (index >= array->size) return NULL;`).
- **TDD note:** neither change is deterministically testable in the current harness (both need crafted malformed fragments the harness doesn't generate). Verified by no-regression on the delivery suite — a deliberate, documented exception.
