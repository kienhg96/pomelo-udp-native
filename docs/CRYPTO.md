# Pomelo Crypto Layer (Agent Reference)

> The bottom layer of the stack (`src/crypto/`): the security root every other layer calls into. Orientation + build/test in [`/CLAUDE.md`](../CLAUDE.md); cross-cutting memory/threading model in [`docs/ARCHITECTURE.md`](ARCHITECTURE.md). This doc covers the code **as merged on `main`** (crypto hardening fixes already applied).

## Role

Thin, stateless wrappers over **libsodium** that provide every cryptographic primitive the engine needs: connect-token sealing (**XChaCha20-Poly1305-ietf**), per-packet AEAD (**ChaCha20-Poly1305-ietf**), a parcel-integrity checksum (**BLAKE2b**), URL-safe base64, and the CSPRNG. No connection state or policy lives here — the protocol/delivery layers own keys, nonces/sequences, and replay. If this layer is wrong, nothing above it is trustworthy, so every entry point is fail-closed and leaves bounds/replay decisions to its callers only where documented below.

## File map

| File | Responsibility |
|------|----------------|
| `src/crypto/token.c` | Connect-token encode/decode (public + XChaCha20-Poly1305 private part), server-address codec, AAD builder, challenge-token encrypt/decrypt (ChaCha20-Poly1305) |
| `src/crypto/token.h` | Internal codec decls + `pomelo_challenge_token_t` (client_id + user_data) |
| `src/crypto/crypto.c` | libsodium init, nonce construction, per-packet AEAD encrypt/decrypt (ChaCha20-Poly1305-ietf) |
| `src/crypto/crypto.h` | AEAD nonce (12) / HMAC (16) byte constants + AEAD/nonce/init decls |
| `src/crypto/checksum.c` | BLAKE2b streaming checksum (init/update/final) over an opaque state blob |
| `src/crypto/checksum.h` | Opaque checksum state (`[384]`) + checksum byte count (32) |
| `src/crypto/random.c` | CSPRNG: `randombytes_buf` + deterministic (seeded) variant |
| `src/crypto/base64.c` | URL-safe base64 encode/decode via libsodium |
| `include/pomelo/token.h` | Public `pomelo_connect_token_t` + public token encode/decode API |
| `include/pomelo/random.h`, `include/pomelo/base64.h` | Public random + base64 API, with base64 length-calc macros |

## Key types & entry points

**Connect tokens** (`token.c`) — the `pomelo_connect_token_t` struct is `include/pomelo/token.h:15`.
- `pomelo_connect_token_encode` — `token.c:17` — build full 2048-byte token (public fields + encrypted private part + padding).
- `pomelo_codec_encode_private_connect_token` — `token.c:95` — XChaCha20-Poly1305-ietf seal of the 1024-byte private part, AAD = version+protocol_id+expire.
- `pomelo_connect_token_decode_private` — `token.c:169` — **server path**, runs on a worker thread via `packet.c:363` (post-auth: only after the server key decrypts). Keys land in `decrypted[]` and are zeroized on every return.
- `pomelo_connect_token_decode_public` — `token.c:397` — **client path** via `client.c:176` (and plugin tokens `api/plugin/token.c:30`); parses unencrypted fields from a backend-issued token.
- `pomelo_codec_decode_server_addresses` — `token.c:338` — bounds-checked address-array decode (shared by both decode paths).
- `pomelo_codec_encode_connect_token_associated_data` — `token.c:370` — the AAD layout both seal and open must agree on.

**Challenge tokens** (`token.c`) — handshake RESPONSE echo.
- `pomelo_codec_encrypt_challenge_token` — `token.c:466` — called `packet.c:402`.
- `pomelo_codec_decrypt_challenge_token` — `token.c:517` — called `packet.c:511`.

**Per-packet AEAD** (`crypto.c`) — the hot path, run on worker threads.
- `pomelo_crypto_make_nonce` — `crypto.c:28` — 12-byte nonce = 4 zero bytes + big-endian-ish packed uint64 sequence.
- `pomelo_crypto_encrypt_aead` — `crypto.c:48` / `pomelo_crypto_decrypt_aead` — `crypto.c:86`. Wrapped by `protocol/crypto.c` (`...encrypt_packet` `:103`, `...decrypt_packet` `:61`), which the protocol sender/receiver offload to the libuv worker threadpool (`protocol/receiver.c:204`, protocol sender).

**Checksum** (`checksum.c`) — BLAKE2b, 32-byte, used by the **delivery** layer for parcel integrity (also on worker threads): `pomelo_crypto_checksum_init` `:6`, `_update` `:24`, `_final` `:40`. Callers: `delivery/receiver.c:302-330`, `delivery/sender.c:206-231`.

**Random** (`random.c`) — `pomelo_random_buffer` `:7` (true CSPRNG), `pomelo_random_buffer_deterministic` `:13` (seeded, reproducible — currently no `src/` caller).

**Init** — `pomelo_crypto_init` — `crypto.c:15` — wraps `sodium_init`; call it once before any other crypto. Invoked at `api/context.c:34`.

## Invariants

- **Every decrypt return is checked (fail-closed).** No plaintext is read until the AEAD/MAC verifies: `token.c:202` (private token), `token.c:549` (challenge token), `crypto.c:114` (per-packet). A failed open returns `< 0`/`-1` and touches no out fields.
- **Private-token AAD binds `version + protocol_id + expire`** (`pomelo_codec_encode_connect_token_associated_data`, `token.c:370`; built identically on seal `token.c:149` and open `token.c:181`). The REQUEST packet header must match this AAD, so a tampered header fails authentication.
- **Nonce uniqueness is the caller's duty.** `make_nonce` is a pure sequence→nonce function (`crypto.c:28`); uniqueness-per-key comes from the protocol's monotonic per-peer sequence + replay window (see [PROTOCOL](PROTOCOL.md)), not from this layer.
- **`naddr` is bounded before the write loop.** `pomelo_codec_decode_server_addresses` rejects `naddr == 0 || naddr > POMELO_CONNECT_TOKEN_MAX_ADDRESSES` (32) and checks the `read_uint32` return (`token.c:351-358`) before writing `addresses + i` — no OOB write from a crafted/corrupt token.
- **Key material is zeroized.** `decode_private` `sodium_memzero`s the stack `decrypted[]` (holding both session keys) on all three exits: decrypt-fail `token.c:203`, address-decode-fail `token.c:224`, success `token.c:249`.
- **Version is validated and rejected.** `decode_public` returns `-1` on version-string mismatch before parsing any field (`token.c:410-413`); it does not report success on a bad token.
- **CSPRNG source is libsodium `randombytes_buf`** (`random.c:9`) — not `rand()`/time-seeded. The deterministic variant fully zeroes its 32-byte seed array and serializes all 8 seed bytes big-endian (`random.c:23-27`) — no uninitialized stack entropy.

## Gotchas

- **Wire version ≠ library version.** The 13-byte version in tokens/REQUEST is the **on-wire** string `"POMELO 1.00"` (`POMELO_VERSION_INFO`, `src/base/constants.h:14`), not the library version `0.0.1`. `decode_public` compares against this constant (`token.c:410`).
- **`*_unsafe` payload helpers assume the caller bounded the buffer.** `make_nonce` uses `pomelo_payload_zero_pad_unsafe(&payload, nonce_length - 8)` and `write_uint64_unsafe` with no runtime guard (`crypto.c:41-44`); it is only safe because every caller passes a 12-byte nonce. `nonce_length < 8` would underflow. Keep the 12-byte contract.
- **`sodium_init` returns `1` when already initialized** — that is **not** an error. Callers must test `< 0`, as `api/context.c:34` does. `pomelo_crypto_init` additionally short-circuits via its own `_initialized` flag (`crypto.c:16`), so the libsodium call happens at most once.
- **Server address decode of an unknown type falls through to IPv6.** `pomelo_codec_decode_server_address` treats any `type != POMELO_ADDRESS_IPV4` as IPv6 (`token.c:313-329`); the byte count differs, so a bogus type shifts subsequent reads. The private path is post-auth, so this is attacker-reachable only with the server's private key.
- **base64 decode does not report the decoded length** (passes `NULL` for `bin_len`, `base64.c:41`) and picks the variant by `b64_len % 4` (`base64.c:44`) — a no-padding string whose length is a multiple of 4 is mis-classified as the padded variant. Known follow-up; fine for the engine's current fixed-size token strings.
- **Checksum state is an opaque blob, size-asserted at init.** `pomelo_crypto_checksum_init` asserts `sizeof(state) >= sizeof(blake2b_state)` (`checksum.c:10-13`); if libsodium's state grows past 384 bytes this trips in debug builds.

## Wire formats

This doc covers the crypto operations, not byte layouts. For the on-wire structures these functions read/write:
- Connect token (public 2048 B + private 1024 B, AAD fields, address list) → [CONNECT_TOKEN.md](CONNECT_TOKEN.md).
- Packet headers, per-packet AEAD framing, HMAC placement, challenge token → [PACKETS.md](PACKETS.md) and [STANDARD.md](STANDARD.md).
