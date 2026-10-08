# Crypto Layer Security Fixes Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Fix the actionable security/correctness findings from the Crypto layer review (`REVIEW.md` §1) with regression tests.

**Architecture:** Pure C, no new dependencies. Each fix is local to `src/crypto/`. Each fix ships with a test in the existing crypto test executable (`test/crypto-test/crypto-test.c`), run via CTest. One commit per task.

**Tech Stack:** C11, libsodium, CMake + CTest. Test harness: `test/pomelo-test.h` (`pomelo_check(cond)` aborts on failure; `pomelo_run_test(fn)` runs a `int fn(void)` returning 0).

---

## Ground rules for the implementer (READ FIRST)

1. **Use the `Write` tool (full-file rewrite), NOT `Edit`.** A linter/hook in this repo reverts partial edits mid-session. Read the whole file, apply your change, write it back in full.
2. **Do NOT `git push`.** Local commits only.
3. **Commit message footer** — every commit message must end with exactly:
   ```
   Co-Authored-By: claude-flow <ruv@ruv.net>
   ```
4. **Build + test commands** (build dir already configured at `build/`):
   ```bash
   cmake --build build --target pomelo-test-crypto -j
   ctest --test-dir build -R pomelo-test-crypto --output-on-failure
   ```
   If the build dir is missing, configure first: `cmake -S . -B build`.
5. **TDD:** write the failing test, see it fail, implement, see it pass, commit. Do not batch.
6. Work on a branch off `main` (current branch is `main`). Create `fix/crypto-security` first:
   ```bash
   git checkout -b fix/crypto-security
   ```

---

## File Structure

| File | Responsibility | Change |
|------|----------------|--------|
| `src/crypto/token.h` | Token codec decls | Change `pomelo_codec_decode_server_addresses` return type `void`→`int` |
| `src/crypto/token.c` | Token codec impl | Guard `naddr`; propagate error in 2 callers; `return -1` on bad version; `sodium_memzero` decrypted buffer |
| `src/crypto/random.c` | Deterministic RNG | Fix seed serialization (full 64-bit, zeroed 32-byte seed) |
| `test/crypto-test/crypto-test.c` | Crypto tests | Add 5 regression test functions + wire into `main()` |

No CMake changes needed — the crypto test already links `${POMELO_CRYPTO}` (compiles `token.c`/`random.c`) and `${POMELO_BASE}` (payload), and includes `${POMELO_INCLUDE}`.

---

## Task 1: Guard `naddr` bound in server-address decoding (🟠 High — OOB write)

**Problem:** `pomelo_codec_decode_server_addresses` reads an attacker/backend-controlled `naddr` (uint32) from the token and loops that many times writing into `addresses[POMELO_CONNECT_TOKEN_MAX_ADDRESSES]` (32) with no bound check → out-of-bounds write.

**Files:**
- Test: `test/crypto-test/crypto-test.c`
- Modify: `src/crypto/token.h` (decl), `src/crypto/token.c:332-350` (impl) + callers at `token.c:217` and `token.c:423`

- [ ] **Step 1: Add includes + failing tests to `test/crypto-test/crypto-test.c`**

At the top of the file, after the existing `#include` lines, ensure these includes are present (add any missing):

```c
#include <string.h>
#include "pomelo-test.h"
#include "pomelo/random.h"
#include "pomelo/address.h"
#include "pomelo/constants.h"
#include "crypto/crypto.h"
#include "crypto/token.h"
#include "base/payload.h"
#include "base/constants.h"
```

Add these two test functions above `main()`:

```c
static int test_decode_addresses_rejects_overflow(void) {
    pomelo_track_function();

    // Craft a payload whose address count exceeds the maximum.
    uint8_t buf[64];
    pomelo_payload_t payload;
    payload.data = buf;
    payload.capacity = sizeof(buf);
    payload.position = 0;
    pomelo_payload_write_uint32(
        &payload,
        (uint32_t) (POMELO_CONNECT_TOKEN_MAX_ADDRESSES + 1)
    );

    payload.position = 0; // rewind for reading
    int naddresses = -1;
    pomelo_address_t addresses[POMELO_CONNECT_TOKEN_MAX_ADDRESSES];
    int ret = pomelo_codec_decode_server_addresses(
        &payload, &naddresses, addresses
    );
    pomelo_check(ret != 0); // must be rejected, not overflow
    return 0;
}

static int test_decode_addresses_roundtrip(void) {
    pomelo_track_function();

    pomelo_connect_token_t token;
    memset(&token, 0, sizeof(token));
    token.naddresses = 2;
    pomelo_check(
        pomelo_address_from_string(&token.addresses[0], "127.0.0.1:8000") == 0
    );
    pomelo_check(
        pomelo_address_from_string(&token.addresses[1], "127.0.0.1:8001") == 0
    );

    uint8_t buf[256];
    pomelo_payload_t payload;
    payload.data = buf;
    payload.capacity = sizeof(buf);
    payload.position = 0;
    pomelo_codec_encode_server_addresses(&payload, &token);

    payload.position = 0; // rewind for reading
    int naddresses = 0;
    pomelo_address_t out[POMELO_CONNECT_TOKEN_MAX_ADDRESSES];
    int ret = pomelo_codec_decode_server_addresses(&payload, &naddresses, out);
    pomelo_check(ret == 0);
    pomelo_check(naddresses == 2);
    return 0;
}
```

Wire them in at the very start of `main()` (right after `pomelo_check(pomelo_crypto_init() == 0);`):

```c
    pomelo_run_test(test_decode_addresses_rejects_overflow);
    pomelo_run_test(test_decode_addresses_roundtrip);
```

- [ ] **Step 2: Build & run — verify the overflow test fails (crash/abort)**

Run:
```bash
cmake --build build --target pomelo-test-crypto -j
ctest --test-dir build -R pomelo-test-crypto --output-on-failure
```
Expected: FAIL — `test_decode_addresses_rejects_overflow` crashes or aborts (OOB write / wild loop) because the bound is not enforced yet.

- [ ] **Step 3: Change the declaration in `src/crypto/token.h`**

Find (around line 49):
```c
void pomelo_codec_decode_server_addresses(
    pomelo_payload_t * payload,
    int * naddresses,
    pomelo_address_t * addresses
);
```
Replace the return type `void` with `int` (keep the parameters identical):
```c
int pomelo_codec_decode_server_addresses(
    pomelo_payload_t * payload,
    int * naddresses,
    pomelo_address_t * addresses
);
```

- [ ] **Step 4: Fix the implementation in `src/crypto/token.c`**

Replace `pomelo_codec_decode_server_addresses` (currently `token.c:332-350`) with:

```c
int pomelo_codec_decode_server_addresses(
    pomelo_payload_t * payload,
    int * naddresses,
    pomelo_address_t * addresses
) {
    assert(payload != NULL);
    assert(naddresses != NULL);
    assert(addresses != NULL);

    *naddresses = 0;

    // number of server addresses
    uint32_t naddr = 0;
    if (pomelo_payload_read_uint32(payload, &naddr) != 0) {
        return -1; // not enough data
    }

    // Reject counts outside the valid range (netcode mandates this).
    if (naddr == 0 || naddr > POMELO_CONNECT_TOKEN_MAX_ADDRESSES) {
        return -1;
    }

    // <for each server address>
    for (uint32_t i = 0; i < naddr; ++i) {
        pomelo_codec_decode_server_address(payload, addresses + i);
    }

    *naddresses = (int) naddr;
    return 0;
}
```

Update the **two callers** to propagate the error (use `Write` on the whole file):

In `pomelo_connect_token_decode_private` (the call currently at `token.c:217`), replace the bare call with:
```c
    // server addresses
    if (pomelo_codec_decode_server_addresses(
            &payload,
            &token->naddresses,
            token->addresses
        ) != 0) {
        return -1;
    }
```

In `pomelo_connect_token_decode_public` (the call currently at `token.c:423`), replace the bare call with:
```c
    // server addresses
    if (pomelo_codec_decode_server_addresses(
            &payload,
            &token->naddresses,
            token->addresses
        ) != 0) {
        return -1;
    }
```

- [ ] **Step 5: Build & run — verify both tests pass**

Run:
```bash
cmake --build build --target pomelo-test-crypto -j
ctest --test-dir build -R pomelo-test-crypto --output-on-failure
```
Expected: PASS — all crypto tests green.

- [ ] **Step 6: Commit**

```bash
git add src/crypto/token.h src/crypto/token.c test/crypto-test/crypto-test.c
git commit -m "fix(crypto): bound server-address count when decoding connect tokens

Reject naddr==0 or naddr>POMELO_CONNECT_TOKEN_MAX_ADDRESSES before the
write loop; propagate the error through decode_public/decode_private.
Prevents out-of-bounds writes to the fixed addresses[32] array.

Co-Authored-By: claude-flow <ruv@ruv.net>"
```

---

## Task 2: Reject bad version info in `decode_public` (🔵 Low — wrong success return)

**Problem:** `pomelo_connect_token_decode_public` returns `0` (success) when the version info does not match, leaving the token unpopulated. A parse routine must not report success on a validation failure.

**Files:**
- Test: `test/crypto-test/crypto-test.c`
- Modify: `src/crypto/token.c:393-396`

- [ ] **Step 1: Add failing test**

Add above `main()`:

```c
static int test_decode_public_rejects_bad_version(void) {
    pomelo_track_function();

    uint8_t buf[POMELO_CONNECT_TOKEN_BYTES];
    memset(buf, 0, sizeof(buf));
    // Corrupt the version-info prefix so it never matches.
    memset(buf, 0xAB, POMELO_VERSION_INFO_BYTES);

    pomelo_connect_token_t token;
    memset(&token, 0, sizeof(token));

    int ret = pomelo_connect_token_decode_public(buf, &token);
    pomelo_check(ret < 0); // bad version must be rejected
    return 0;
}
```

Wire into `main()` (after the Task 1 lines):
```c
    pomelo_run_test(test_decode_public_rejects_bad_version);
```

- [ ] **Step 2: Build & run — verify it fails**

Run:
```bash
cmake --build build --target pomelo-test-crypto -j
ctest --test-dir build -R pomelo-test-crypto --output-on-failure
```
Expected: FAIL — `decode_public` returns `0` on version mismatch, so `pomelo_check(ret < 0)` aborts.

- [ ] **Step 3: Fix `src/crypto/token.c`**

In `pomelo_connect_token_decode_public`, find:
```c
    int cmp = memcmp(buffer, POMELO_VERSION_INFO, POMELO_VERSION_INFO_BYTES);
    if (cmp != 0) { // Not equals
        return 0;
    }
```
Replace the `return 0;` with `return -1;`:
```c
    int cmp = memcmp(buffer, POMELO_VERSION_INFO, POMELO_VERSION_INFO_BYTES);
    if (cmp != 0) { // Not equals -> invalid token
        return -1;
    }
```

- [ ] **Step 4: Build & run — verify pass**

Run:
```bash
cmake --build build --target pomelo-test-crypto -j
ctest --test-dir build -R pomelo-test-crypto --output-on-failure
```
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add src/crypto/token.c test/crypto-test/crypto-test.c
git commit -m "fix(crypto): reject connect token with mismatched version info

decode_public returned success (0) on version mismatch, leaving the
token struct unpopulated. Return -1 so callers reject it.

Co-Authored-By: claude-flow <ruv@ruv.net>"
```

---

## Task 3: Fix `pomelo_random_buffer_deterministic` (🟡 Medium — broken seed / uninitialized stack)

**Problem:** (a) `seed = (seed >> 32) & (seed & 0xFFFFFFFFULL)` destroys entropy by AND-ing the high and low halves; (b) only 4 of the 32 (`randombytes_SEEDBYTES`) seed bytes are set, so 28 bytes of uninitialized stack feed the deterministic RNG → non-deterministic, UB.

**Files:**
- Test: `test/crypto-test/crypto-test.c`
- Modify: `src/crypto/random.c`

- [ ] **Step 1: Add failing tests**

Add above `main()`:

```c
static int test_random_deterministic_uses_full_seed(void) {
    pomelo_track_function();

    // Two distinct 64-bit seeds that collapse to the SAME value under the
    // old broken (high32 & low32) mixing, but differ as full 64-bit seeds.
    uint8_t a[64];
    uint8_t b[64];
    pomelo_random_buffer_deterministic(a, sizeof(a), 0xFFFFFFFF00000001ULL);
    pomelo_random_buffer_deterministic(b, sizeof(b), 0x00000001FFFFFFFFULL);
    pomelo_check(memcmp(a, b, sizeof(a)) != 0); // full seed must matter
    return 0;
}

static int test_random_deterministic_is_repeatable(void) {
    pomelo_track_function();

    uint8_t a[64];
    uint8_t b[64];
    pomelo_random_buffer_deterministic(a, sizeof(a), 0x123456789ABCDEF0ULL);
    pomelo_random_buffer_deterministic(b, sizeof(b), 0x123456789ABCDEF0ULL);
    pomelo_check(memcmp(a, b, sizeof(a)) == 0); // same seed -> same output
    return 0;
}
```

Wire into `main()`:
```c
    pomelo_run_test(test_random_deterministic_uses_full_seed);
    pomelo_run_test(test_random_deterministic_is_repeatable);
```

- [ ] **Step 2: Build & run — verify `uses_full_seed` fails**

Run:
```bash
cmake --build build --target pomelo-test-crypto -j
ctest --test-dir build -R pomelo-test-crypto --output-on-failure
```
Expected: FAIL — both seeds collapse to identical 4 seed bytes under the broken mixing, producing equal buffers, so `pomelo_check(memcmp != 0)` aborts.

- [ ] **Step 3: Fix `src/crypto/random.c`**

Replace the entire file contents with:

```c
#include <assert.h>
#include <string.h>
#include "pomelo/random.h"
#include "sodium/randombytes.h"


void pomelo_random_buffer(void * buffer, size_t length) {
    assert(buffer != NULL);
    randombytes_buf(buffer, length);
}


void pomelo_random_buffer_deterministic(
    void * buffer,
    size_t length,
    uint64_t seed
) {
    assert(buffer != NULL);
    if (length == 0) return; // Empty

    // Serialize the full 64-bit seed (big-endian) into a fully-zeroed
    // seed array of randombytes_SEEDBYTES bytes.
    uint8_t seed_arr[randombytes_SEEDBYTES];
    memset(seed_arr, 0, sizeof(seed_arr));
    for (int i = 0; i < 8; ++i) {
        seed_arr[i] = (uint8_t) ((seed >> (56 - i * 8)) & 0xFF);
    }

    randombytes_buf_deterministic(buffer, length, seed_arr);
}
```

- [ ] **Step 4: Build & run — verify pass**

Run:
```bash
cmake --build build --target pomelo-test-crypto -j
ctest --test-dir build -R pomelo-test-crypto --output-on-failure
```
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add src/crypto/random.c test/crypto-test/crypto-test.c
git commit -m "fix(crypto): correct deterministic RNG seed handling

Previous code AND-ed the high and low halves of the seed (destroying
entropy) and left 28 of 32 seed bytes uninitialized. Zero the seed
array and serialize the full 64-bit seed big-endian.

Co-Authored-By: claude-flow <ruv@ruv.net>"
```

---

## Task 4: Zeroize decrypted key material on the stack (🟡 Medium — key lingering)

**Problem:** `pomelo_connect_token_decode_private` holds the decrypted private token (including `client_to_server_key` / `server_to_client_key`) in the local `decrypted[]` buffer and never wipes it before returning.

**Files:**
- Test: `test/crypto-test/crypto-test.c` (regression round-trip; stack zeroization itself can't be asserted directly — the test guards that the fix doesn't break decoding)
- Modify: `src/crypto/token.c` (`pomelo_connect_token_decode_private`)

- [ ] **Step 1: Add round-trip regression test**

Add above `main()`:

```c
static int test_token_private_roundtrip(void) {
    pomelo_track_function();

    uint8_t key[POMELO_KEY_BYTES];
    pomelo_random_buffer(key, sizeof(key));

    pomelo_connect_token_t in;
    memset(&in, 0, sizeof(in));
    in.protocol_id = 0x1122334455667788ULL;
    in.expire_timestamp = 9999999999ULL;
    in.client_id = 0x00000000CAFEBABEULL;
    in.timeout = 10;
    in.naddresses = 1;
    pomelo_check(
        pomelo_address_from_string(&in.addresses[0], "127.0.0.1:9000") == 0
    );
    pomelo_random_buffer(in.connect_token_nonce, POMELO_CONNECT_TOKEN_NONCE_BYTES);
    pomelo_random_buffer(in.client_to_server_key, POMELO_KEY_BYTES);
    pomelo_random_buffer(in.server_to_client_key, POMELO_KEY_BYTES);
    pomelo_random_buffer(in.user_data, POMELO_USER_DATA_BYTES);

    uint8_t priv[POMELO_CONNECT_TOKEN_PRIVATE_BYTES];
    int ret = pomelo_codec_encode_private_connect_token(priv, &in, key);
    pomelo_check(ret == 0);

    pomelo_connect_token_t out;
    memset(&out, 0, sizeof(out));
    // AAD inputs must match what was used at encryption time.
    out.protocol_id = in.protocol_id;
    out.expire_timestamp = in.expire_timestamp;
    memcpy(
        out.connect_token_nonce,
        in.connect_token_nonce,
        POMELO_CONNECT_TOKEN_NONCE_BYTES
    );

    ret = pomelo_connect_token_decode_private(priv, &out, key);
    pomelo_check(ret == 0);
    pomelo_check(out.client_id == in.client_id);
    pomelo_check(out.naddresses == 1);
    pomelo_check(
        memcmp(out.client_to_server_key, in.client_to_server_key, POMELO_KEY_BYTES) == 0
    );
    return 0;
}
```

Wire into `main()`:
```c
    pomelo_run_test(test_token_private_roundtrip);
```

- [ ] **Step 2: Build & run — verify the round-trip passes BEFORE the zeroize change**

Run:
```bash
cmake --build build --target pomelo-test-crypto -j
ctest --test-dir build -R pomelo-test-crypto --output-on-failure
```
Expected: PASS (this test documents current correct decode behavior; the zeroize change must keep it green).

- [ ] **Step 3: Add zeroization in `src/crypto/token.c`**

In `pomelo_connect_token_decode_private`, after the decrypt failure check and at the successful return, wipe the `decrypted` buffer. The function currently returns early on `ret != 0` and falls through to `return 0;` at the end. Make BOTH exits wipe the buffer:

Replace:
```c
    if (ret != 0) {
        return ret;
    }
```
with:
```c
    if (ret != 0) {
        sodium_memzero(decrypted, sizeof(decrypted));
        return ret;
    }
```

And immediately before the final `return 0;` of the function, add:
```c
    sodium_memzero(decrypted, sizeof(decrypted));
    return 0;
```

Ensure the sodium utils header is included at the top of `token.c` (add if missing):
```c
#include "sodium/utils.h"
```

- [ ] **Step 4: Build & run — verify still passes**

Run:
```bash
cmake --build build --target pomelo-test-crypto -j
ctest --test-dir build -R pomelo-test-crypto --output-on-failure
```
Expected: PASS — decode still works; keys are now wiped from the stack buffer.

- [ ] **Step 5: Commit**

```bash
git add src/crypto/token.c test/crypto-test/crypto-test.c
git commit -m "fix(crypto): zeroize decrypted connect-token buffer after use

The local decrypted[] buffer holds session keys after decryption and
was never wiped. sodium_memzero it on both exits.

Co-Authored-By: claude-flow <ruv@ruv.net>"
```

---

## Out of scope (follow-up, do NOT do in this patch)

These are noted in `REVIEW.md` §1 but intentionally excluded to keep the patch focused:
- 🔵 `base64.c:36` — `pomelo_base64_decode` passes `NULL` for the decoded length and uses a fragile variant heuristic. Changing the signature touches its caller (`http_client`); handle separately.
- ⚪ `crypto.c:41` — `zero_pad_unsafe(nonce_length - 8)` underflow potential; safe with current callers (nonce always 12B).

---

## Self-Review (completed by plan author)

- **Spec coverage:** Tasks 1–4 cover the 🟠 and both 🟡 findings plus the 🔵 version-return finding from `REVIEW.md` §1. base64 + nit explicitly deferred.
- **Placeholder scan:** none — every step has concrete code and commands.
- **Type consistency:** `pomelo_codec_decode_server_addresses` signature changed in both `token.h` and `token.c`; both callers updated. Test helper names are unique and match their `pomelo_run_test` wiring.
- **Risk:** signature change is internal (2 callers, both in `token.c`); no public header affected.
