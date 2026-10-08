# Protocol Layer Hardening Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Fix the Protocol layer findings from `REVIEW.md` §3 — bound the number of half-open (pending) handshake peers to blunt a spoofed-REQUEST flood, and add a defensive underflow guard in packet length validation.

**Architecture:** Pure C. Two localized changes in `src/protocol/`. The cap relies on a protocol fact: clients retransmit their REQUEST on an interval when they get no response, so dropping an excess REQUEST at the cap is safe — the client simply retries once a slot frees.

**Tech Stack:** C11, CMake + CTest. Harness `test/pomelo-test.h`.

---

## Ground rules for the implementer (READ FIRST)

1. **Use the `Write` tool (full-file rewrite), NOT `Edit`.** A linter/hook in this repo reverts partial edits. Read the whole file, apply the change, write it back in full. `src/protocol/server.c` is large (~34 KB) — read it fully before rewriting.
2. **Do NOT `git push`.** Local commits only.
3. Every commit message MUST end with exactly:
   ```
   Co-Authored-By: claude-flow <ruv@ruv.net>
   ```
4. Branch off `main` first:
   ```bash
   git checkout main
   git checkout -b fix/protocol-hardening
   ```
   (Untracked files like REVIEW.md and docs/superpowers/ carry over — leave them, do not commit them.)
5. Build + test commands (build dir already configured at `build/`):
   ```bash
   cmake --build build --target pomelo-test-protocol-server pomelo-test-protocol-client pomelo-test-protocol pomelo-test-protocol-packet -j
   ctest --test-dir build -R "pomelo-test-protocol" --output-on-failure
   ```
   Note: the target `pomelo-test-general` is known-broken in this working tree (its source file is missing from a pre-existing modified `CMakeLists.txt`) — ignore it; do not try to fix it.
6. One commit per task. Run the protocol tests after every task.
7. **No failing-first test for these tasks.** Both changes preserve existing behavior on all current inputs (the cap only triggers under a flood of distinct-address REQUESTs, which the test harness does not generate; the underflow guard rejects the same inputs the `== EXPECTED` checks already reject). Verify by: the full protocol test suite stays green (no regression), and re-read the diff. This is a deliberate, documented exception to TDD — do not fabricate a flaky flood test.

---

## Task 1: Cap half-open (pending) handshake peers (🟡 Medium — anti-DoS)

**Problem:** `protocol_id` is a public constant, so an attacker can send well-formed REQUEST packets (no valid token) from spoofed source addresses. In `pomelo_protocol_server_validate`, each new-address REQUEST that passes the cheap `protocol_id` check allocates a peer into `requesting_peers` **and** submits an XChaCha20 decryption worker task, *before* the token is shown to be garbage. Only `connected_peers` is bounded (`max_clients`); `requesting_peers` is unbounded and not age-reaped, so a flood faster than worker throughput grows `requesting_peers` + `peer_pool` without limit (memory/CPU DoS).

**Fix:** Before allocating a peer for a new REQUEST, drop the packet if `requesting_peers->size >= max_clients`. Only `requesting_peers` needs a cap: `challenging_peers` can only be populated from a successfully decrypted (valid) token, which an attacker cannot forge. Dropping is safe because legitimate clients retransmit their REQUEST.

**Files:**
- Modify: `src/protocol/server.c` (`pomelo_protocol_server_validate`, the `if (!peer)` block at ~line 313)

- [ ] **Step 1: Read `src/protocol/server.c` fully.** Locate the new-peer allocation inside `pomelo_protocol_server_validate`. It currently reads:

```c
    // Create new anonymous peer
    if (!peer) {
        peer = pomelo_protocol_server_acquire_peer(server, address);
        if (!peer) {
            return -1; // Cannot allocate new anonymous peer
        }

        peer->entry = pomelo_list_push_back(server->requesting_peers, peer);
        if (!peer->entry) {
            // Error when trying to move peer to anonymous list, release peer
            pomelo_protocol_server_release_peer(server, peer);
            return -1;
        }

        peer->state = POMELO_PROTOCOL_PEER_REQUEST;
    }
```

- [ ] **Step 2: Add the pending-peer cap**

Rewrite that block (via `Write` on the whole file) so the cap check comes first:

```c
    // Create new anonymous peer
    if (!peer) {
        // Bound the number of half-open (pending) handshakes. An unauthenticated
        // REQUEST (protocol_id is public) otherwise allocates a peer and a
        // decryption worker task before the token is validated, so a spoofed
        // flood could exhaust memory/CPU. Only requesting_peers needs this cap:
        // challenging_peers is only reachable with a valid (decrypted) token.
        // Dropping is safe because clients retransmit their REQUEST.
        if (server->requesting_peers->size >= server->max_clients) {
            return -1; // Too many pending handshakes; drop this REQUEST
        }

        peer = pomelo_protocol_server_acquire_peer(server, address);
        if (!peer) {
            return -1; // Cannot allocate new anonymous peer
        }

        peer->entry = pomelo_list_push_back(server->requesting_peers, peer);
        if (!peer->entry) {
            // Error when trying to move peer to anonymous list, release peer
            pomelo_protocol_server_release_peer(server, peer);
            return -1;
        }

        peer->state = POMELO_PROTOCOL_PEER_REQUEST;
    }
```

(Do not change anything else in the function or file.)

- [ ] **Step 3: Build & run protocol tests — verify no regression**

Run:
```bash
cmake --build build --target pomelo-test-protocol-server pomelo-test-protocol-client pomelo-test-protocol pomelo-test-protocol-packet -j
ctest --test-dir build -R "pomelo-test-protocol" --output-on-failure
```
Expected: PASS — normal handshakes are well under `max_clients` pending at a time, so the cap never triggers in the tests.

- [ ] **Step 4: Commit**

```bash
git add src/protocol/server.c
git commit -m "fix(protocol): cap half-open handshake peers to blunt REQUEST flood

An unauthenticated REQUEST (protocol_id is public) allocated a peer and
a decryption worker task before the token was validated, with no bound
on requesting_peers. Drop new REQUESTs once requesting_peers reaches
max_clients; legitimate clients retransmit. challenging_peers needs no
cap since it requires a valid decrypted token.

Co-Authored-By: claude-flow <ruv@ruv.net>"
```

---

## Task 2: Guard `size_t` underflow in `validate_body_length` (⚪ Nit)

**Problem:** `pomelo_protocol_packet_validate_body_length` computes `length -= POMELO_HMAC_BYTES` without checking `body_length >= POMELO_HMAC_BYTES`. The result is not currently exploitable (an underflowed `length` fails every `length == EXPECTED_SIZE` branch and the packet is rejected), but the subtraction is undefined-intent and should be guarded explicitly.

**Files:**
- Modify: `src/protocol/packet.c` (`pomelo_protocol_packet_validate_body_length`, ~lines 217-225)

- [ ] **Step 1: Read the function.** It currently begins:

```c
bool pomelo_protocol_packet_validate_body_length(
    pomelo_protocol_packet_type type,
    size_t body_length,
    bool encrypted
) {
    size_t length = body_length;
    if (encrypted && type != POMELO_PROTOCOL_PACKET_REQUEST) {
        length -= POMELO_HMAC_BYTES;
    }
    switch (type) {
        ...
```

- [ ] **Step 2: Add the underflow guard**

Change the `if` block so an undersized encrypted body is rejected before the subtraction:

```c
    size_t length = body_length;
    if (encrypted && type != POMELO_PROTOCOL_PACKET_REQUEST) {
        if (body_length < POMELO_HMAC_BYTES) {
            return false; // Too short to contain the auth tag
        }
        length -= POMELO_HMAC_BYTES;
    }
```

(Rewrite the whole file with `Write`, preserving everything else.)

- [ ] **Step 3: Build & run protocol tests — verify no regression**

Run:
```bash
cmake --build build --target pomelo-test-protocol-packet pomelo-test-protocol -j
ctest --test-dir build -R "pomelo-test-protocol" --output-on-failure
```
Expected: PASS — the guard rejects exactly the undersized inputs the existing `== EXPECTED_SIZE` checks already rejected.

- [ ] **Step 4: Commit**

```bash
git add src/protocol/packet.c
git commit -m "harden(protocol): guard body-length underflow before HMAC subtract

validate_body_length subtracted POMELO_HMAC_BYTES without checking the
body was at least that long. Not exploitable today (the wrapped length
fails every size check), but guard it explicitly.

Co-Authored-By: claude-flow <ruv@ruv.net>"
```

---

## Final verification

- [ ] Run the protocol suite once more to confirm all green:
```bash
ctest --test-dir build -R "pomelo-test-protocol" --output-on-failure
```
- [ ] `git log --oneline` should show 2 new commits on `fix/protocol-hardening`. Do NOT push.

---

## Out of scope (follow-up)

- The cap value is fixed at `max_clients`. If the architect wants headroom for large simultaneous-connect bursts, this could become a configurable server option later.
- Optional age-based reaping of `requesting_peers` was considered unnecessary: every requesting peer's async decode completes and removes it, and the cap already bounds the steady-state count.

---

## Self-Review (completed by plan author)

- **Spec coverage:** Task 1 = 🟡 half-open flood cap; Task 2 = ⚪ underflow nit. Both §3 findings covered.
- **Placeholder scan:** none — exact code and commands throughout.
- **Type consistency:** `server->requesting_peers->size` and `server->max_clients` are both `size_t` (server.h) — comparison is well-typed. `POMELO_HMAC_BYTES` is the same constant already used on the line being guarded.
- **TDD note:** neither change is deterministically testable in the current harness (cap needs a distinct-address flood; the guard preserves observable behavior). Verified by no-regression on the protocol suite — a deliberate, documented exception.
