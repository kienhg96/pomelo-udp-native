# Pomelo UDP Native — Code Review

> Review theo hướng **bottom-up**: nền móng (crypto, platform) trước, vì nếu nền sai thì các layer trên vô nghĩa.
> Mỗi layer: Scope → Checklist → Findings. Findings xếp theo mức độ: 🔴 Critical · 🟠 High · 🟡 Medium · 🔵 Low · ⚪ Nit.
> Mỗi finding ghi `file:line`, mô tả 1 câu, và kịch bản lỗi cụ thể. Trạng thái: ☐ chưa soi · 🔄 đang soi · ✅ xong.

| # | Layer | Thư mục | Trạng thái |
|---|-------|---------|-----------|
| 1 | Crypto | `src/crypto/`, `include/pomelo/{token,random,base64}.h` | ✅ |
| 2 | Platform | `src/platform/`, `src/platform/uv/`, `include/pomelo/platform.h` | ✅ |
| 3 | Protocol | `src/protocol/` | ✅ |
| 4 | Delivery | `src/delivery/` | ✅ |
| 5 | API | `src/api/`, `include/pomelo/api.h` | ✅ |
| 6 | Bug báo về từ pomelo-udp-node | `src/platform/uv/udp.c`, `src/api/context.c` | N1 ✅ đã vá · N2 ✅ đã vá · N3 ✅ đã vá |

---

## 1. Crypto Layer  ✅ (đã review + đã vá)

> **Trạng thái vá:** 4 finding dưới (🟠 naddr, 🟡 zeroize, 🟡 random, 🔵 version) đã được vá trên branch `fix/crypto-security` (4 commit, TDD, chưa push). Đã verify độc lập: build OK, `ctest pomelo-test-crypto` 100% passed. Finding 🔵 base64 + ⚪ nit để lại follow-up. Xem `docs/superpowers/plans/2026-10-08-crypto-security-fixes.md`.

**Vai trò:** Token (XChaCha20-Poly1305), mã hoá per-packet (ChaCha20-Poly1305-ietf), checksum (BLAKE2b), base64, random. Gốc bảo mật của toàn hệ thống.

### Findings

**🟠 High — Thiếu chặn `naddr` khi decode server addresses → ghi tràn mảng (OOB write)**
`src/crypto/token.c:343-349` (`pomelo_codec_decode_server_addresses`)
`naddr` đọc từ token (uint32, nguồn ngoài) **không kiểm `<= POMELO_CONNECT_TOKEN_MAX_ADDRESSES (32)`**; vòng lặp ghi `addresses + i` cho tới `naddr`. `read_uint32` có bounds-check nhưng return bị bỏ qua (dòng 343) và `pomelo_address_set` vẫn ghi `addresses+i`.
- Path client — `decode_public` ← `client.c:176`: token từ backend; backend lỗi/MITM/hỏng → OOB write, crash/memory corruption trên client.
- Path server — `decode_private` ← `packet.c:360`: **hậu-auth** (chỉ chạy sau khi giải mã thành công bằng private key) → defense-in-depth.
- netcode.io bắt buộc `1 <= num <= MAX`; code này thiếu.
- **Fix:** trong cả `decode_public` và `decode_server_addresses`, reject nếu `naddr == 0 || naddr > POMELO_CONNECT_TOKEN_MAX_ADDRESSES` trước vòng lặp; đồng thời kiểm return của các `payload_read_*`.

**🟡 Medium — Key material không được zeroize trên stack**
`src/crypto/token.c:186` (`pomelo_connect_token_decode_private`)
Buffer cục bộ `decrypted[POMELO_CONNECT_TOKEN_PRIVATE_BYTES]` chứa `client_to_server_key` / `server_to_client_key` sau giải mã nhưng **không `sodium_memzero`** trước khi return → key nằm lại trên stack (có thể bị đọc qua lỗ hổng khác / core dump).
- **Fix:** `sodium_memzero(decrypted, sizeof(decrypted))` cuối hàm (kể cả nhánh lỗi).

**🟡 Medium — `pomelo_random_buffer_deterministic` hỏng (mìn hẹn giờ)**
`src/crypto/random.c:20-28`
(a) `seed = (seed >> 32) & (seed & 0xFFFFFFFFULL)` — trộn seed bằng `&` làm mất gần hết entropy (nghi lỗi gõ, đáng lẽ `^`/`|`); (b) chỉ set 4/32 byte của `seed_arr[randombytes_SEEDBYTES]` → **28 byte seed là stack chưa khởi tạo** đưa vào `randombytes_buf_deterministic`.
- Hiện **chưa có caller nào trong `src/`** (dead code) → chưa gây hại thực tế, nhưng ai dùng sau sẽ ra output không-deterministic phụ thuộc rác stack.
- **Fix:** memset `seed_arr` = 0 rồi điền đủ, sửa phép trộn seed; hoặc xoá hàm nếu không dùng.

**🔵 Low — `decode_public` trả 0 (success) khi version info sai**
`src/crypto/token.c:393-396` (`pomelo_connect_token_decode_public`)
Hàm parse trả "thành công" trên lỗi validate (version mismatch → `return 0`), không đọc field nào. Hiện được cứu nhờ check downstream (`client.c:186` expire, `:191` naddresses) với điều kiện struct zero-init. Semantics sai, dễ gây bug về sau.
- **Fix:** `return -1` khi version mismatch.

**🔵 Low — `base64_decode` không trả độ dài giải mã + heuristic variant mong manh**
`src/crypto/base64.c:36-47`
Truyền `NULL` cho `bin_len` → caller không biết số byte thực giải mã. Heuristic chọn variant theo `b64_len % 4` dễ sai với chuỗi no-padding có độ dài chia hết 4.

**⚪ Nit — Dùng `*_unsafe`/assert thay kiểm tra runtime trong crypto core**
`src/crypto/crypto.c:41` (`zero_pad_unsafe(nonce_length - 8)` underflow nếu `nonce_length < 8`), `:58-62` (assert thay vì return lỗi). An toàn với caller nội bộ hiện tại (nonce luôn 12B) nhưng mong manh.

### Điểm tốt
- AEAD đúng chuẩn: ChaCha20-Poly1305-ietf (per-packet), XChaCha20-Poly1305-ietf (token); **decrypt luôn kiểm return → fail-closed** (`token.c:201, 530`, `crypto.c:114`).
- AAD của private token bind `version + protocol_id + expire`; header REQUEST phải khớp AAD (`packet.c:351-352`) → chống sửa header.
- Random thật từ `randombytes_buf` (CSPRNG libsodium).
- `checksum_init` assert opaque-size khớp BLAKE2b state (defensive).
- `crypto_init` caller kiểm `< 0` đúng (`context.c:34`) — `sodium_init` trả 1 khi đã init là hợp lệ.

---

## 2. Platform Layer  ✅ (đã review + đã vá)

> **Trạng thái vá:** finding 🟠 (NULL recv buffer), 🟡 (idle_close), 🟡 (executor race), 🔵 (udp niovec/length) đã vá trên branch `fix/platform-hardening` (5 commit, chưa push). Verify độc lập: build OK, `pomelo-test-platform-uv` + `api-basic` + `api-broadcast` đều PASS. 🔵 destroy-before-close để follow-up. Xem `docs/superpowers/plans/2026-10-08-platform-fixes.md`.
> **Lưu ý (ngoài scope):** target `pomelo-test-general` không build được do `CMakeLists.txt` (đang modified sẵn trong working tree) tham chiếu `test/general-test/general-test.c` KHÔNG tồn tại — lỗi có sẵn, không do review.

**Vai trò:** Trừu tượng hoá I/O bất đồng bộ trên libuv: UDP socket, timer, worker threadpool (offload crypto), executor (submit task xuyên thread qua `uv_async`), time. Adapter (`src/adapter/default.c`) là cầu nối platform↔protocol cho recv/send.

### Findings

**🟠 High — Crash khi pool buffer cạn trên recv path (DoS)**
`src/adapter/default.c:216,229` + `src/platform/uv/udp.c:415-418,438-440`
`pomelo_buffer_from_data(data)` là macro `((pomelo_buffer_t *)(data)) - 1`. Khi `alloc_callback` không cấp được buffer (`pomelo_adapter_buffer_acquire` trả NULL do pool cạn/OOM), `iovec.data` để NULL; libuv gọi recv_cb với `nread < 0, buf.base = NULL`; adapter `recv_callback` (cờ STARTED vẫn bật nên **không** early-return) chạy `pomelo_buffer_from_data(NULL)` = `(pomelo_buffer_t *)0 - 1` (con trỏ rác) rồi `pomelo_buffer_unref()` trên đó → đọc/ghi `ref_counter` tại địa chỉ rác → **crash / memory corruption**.
- Kịch bản: attacker flood gói làm cạn buffer pool (hoặc OOM) → mỗi gói đến sau đó crash.
- **Fix:** đầu `pomelo_adapter_recv_callback`, thêm `if (!iovec->data) return;` trước khi gọi `from_data`.

**🟡 Medium — `idle_close` handle không bao giờ `uv_close`**
`src/platform/uv/platform-uv.c:52,148`
`uv_idle_t idle_close` được `uv_idle_init` trên loop của caller nhưng `idle_close_cb` chỉ `uv_idle_stop`, không `uv_close`; `pomelo_platform_uv_destroy` cũng không đóng. Sau khi platform bị free, loop của caller vẫn giữ tham chiếu tới `&platform->idle_close` (vùng nhớ đã free) → `uv_loop_close` trả `EBUSY` / UAF khi loop chạy tiếp.
- **Fix:** `uv_close((uv_handle_t *)&platform->idle_close, ...)` trong quá trình đóng, chờ close cb rồi mới free platform.

**🟡 Medium — Race `executor_submit` vs `shutdown`**
`src/platform/uv/executor.c:223-254` vs `:422-443`
`pomelo_platform_uv_executor_submit` (gọi được từ thread khác) kiểm `executor->running` rồi mới khoá mutex + `uv_async_send`. Nếu `shutdown` chạy xen giữa (CAS running→false, `uv_close` async, release executor), submit có thể `uv_async_send` lên handle đã đóng và push task vào executor sắp bị release → UAF/leak. Hợp đồng ngầm là owner phải ngừng submit trước khi release, nhưng không được enforce.

**🔵 Low — `destroy()` không ép buộc `close()` xong trước**
`src/platform/uv/platform-uv.c:92-117`
`pomelo_platform_uv_destroy` free thẳng pool/list backing cho socket/timer/task trong khi các uv handle có thể còn mở trên loop → UAF nếu gọi sai thứ tự. Nên `assert` trạng thái đã đóng hoặc tự chạy close trước.

**🔵 Low — `udp_send` thiếu chặn biên `niovec`**
`src/platform/uv/udp.c:334-359`
`bufs[POMELO_PLATFORM_UDP_MAX_NUMBER_BUF_VECTORS]` chỉ điền tối đa 2 phần tử nhưng `uv_udp_send` được gọi với `niovec` thật; nếu caller truyền `niovec > 2` → libuv đọc `bufs[2+]` chưa khởi tạo (OOB read). Nội bộ hiện luôn ≤ 2 nhưng nên `assert(niovec <= MAX)`.

**⚪ Nit — `recv_callback` gán `iovec.length = (size_t) nread` khi `nread < 0`**
`src/platform/uv/udp.c:440` → thành số khổng lồ; được cứu vì adapter kiểm `status == 0` trước khi dùng `length` (và fix NULL-data ở trên sẽ short-circuit). Nên gán `length = 0` khi `nread <= 0`.

### Điểm tốt
- Shutdown async đúng chuẩn: dùng callback của `uv_close` để defer release (timer/socket/executor/async), gom `closed_components` bitmask rồi mới gọi callback người dùng (`platform-uv.c:223-232`).
- Ranh giới thread rõ: worker `entry` chạy ở threadpool, `complete` ở loop thread (`worker.c:184-213`); executor double-buffer `tasks_front/back` + mutex cho submit xuyên thread; pool executor `synchronized=true`.
- Mọi handle đều được detach khỏi `pomelo_platform_handle_t` khi đóng → chống tái dùng handle đã giải phóng.
- Timer không-lặp: release được defer vào `uv_timer_stop_complete` (callback của `uv_close`), không free ngay trong callback → an toàn.

---

## 3. Protocol Layer  ✅ (đã review + đã vá)

> **Trạng thái vá:** 🟡 half-open cap + ⚪ underflow guard đã vá trên branch `fix/protocol-hardening` (2 commit, chưa push). Verify độc lập: 5/5 protocol tests PASS. Xem `docs/superpowers/plans/2026-10-08-protocol-hardening.md`.

**Vai trò:** Handshake state machine (REQUEST→CHALLENGE→RESPONSE→CONNECTED), peer/connection, encrypt per-packet, replay protection, anti-DoS.

### Findings

**🟡 Medium — Không giới hạn / không reap theo thời gian các half-open peer (anti-DoS)**
`src/protocol/server.c:313-327` (cấp peer trong `validate`) + `scan_challenging_peers` (`server.c:1058`)
`protocol_id` là hằng số công khai. Mỗi REQUEST chưa-xác-thực từ địa chỉ mới (spoof được) qua quick-check protocol_id → **cấp một peer vào `requesting_peers` + submit tác vụ giải mã XChaCha20 lên worker** TRƯỚC khi token được xác thực.
- Chỉ `connected_peers` bị chặn bởi `max_clients` (`server.c:440`). `requesting_peers` **không reap theo tuổi** (chỉ dựa async decode hoàn tất); chỉ `challenging_peers` reap định kỳ bằng timer; `denied_peers` release sau khi gửi DENIED.
- Flood REQUEST spoof nhanh hơn throughput worker → `requesting_peers` + `peer_pool` phình không giới hạn → **DoS bộ nhớ/CPU**.
- **Giảm thiểu đã có:** không amplification (REQUEST bắt buộc full-size 1078B, DENIED nhỏ hơn), quick-check protocol_id trước khi cấp peer. Một phần là bản chất UDP connectionless, nhưng netcode tham chiếu cap tổng số slot (gồm pending).
- **Khuyến nghị:** cap tổng pending peer (requesting+challenging) theo `max_clients`, và/hoặc reap `requesting_peers` theo tuổi.

**⚪ Nit — Underflow `size_t` trong `validate_body_length`**
`src/protocol/packet.c:224` — `length -= POMELO_HMAC_BYTES` không kiểm `body_length >= POMELO_HMAC_BYTES`. **KHÔNG khai thác được**: sau underflow giá trị wrap khổng lồ, mọi nhánh `length == EXPECTED_SIZE` đều false → reject. Chỉ nên thêm guard cho sạch.

### Điểm tốt (đã verify)
- **Replay protection đúng** (`peer.c:170-196`): `received_sequence[]` init `UINT64_MAX` (`memset 0xff`), sliding window chuẩn netcode, window 256; gói sequence 0 chấp nhận đúng.
- **State machine siết chặt**: mọi `recv_*` kiểm đúng state kỳ vọng và bỏ qua nếu sai → không hỏng trạng thái do gói sai thứ tự.
- **Token gate đúng**: key phiên + protocol_id chỉ nạp vào `crypto_ctx` từ private token **giải mã thành công** (`server.c:460-470`); attacker không có private key không vượt được CHALLENGE.
- **Cờ `PROCESSING_RESPONSE` đúng cặp** (set `socket.c:520` / clear `socket.c:452`) → chống race decode RESPONSE đồng thời; RESPONSE gửi lại được xử lý bằng state check (không kẹt).
- **Không amplification**: REQUEST yêu cầu đủ body size (`packet.c:323`); CHALLENGE/DENIED ≤ REQUEST.
- `recv_response` kiểm cả `client_id` lẫn `user_data` khớp challenge token trước khi connect (`server.c:519-529`).
- Mọi chuyển `peer->state` chạy trên loop thread; crypto offload sang worker trên buffer độc lập → nghi vấn "race state" là **false positive**.

---

## 4. Delivery Layer  ✅ (đã review + đã vá)

> **Trạng thái vá:** 🟠 OOB fragment_index crash + 🟡 cap last_index đã vá trên branch `fix/delivery-hardening` (2 commit, chưa push). Verify độc lập: 2/2 delivery tests PASS, `assert(fragment != NULL)` nguy hiểm đã được thay bằng guard. 🟡 HOL blocking để cho nhánh `dev/reliable-parallel`. Xem `docs/superpowers/plans/2026-10-08-delivery-fixes.md`.

**Vai trò:** Reliable/Sequenced/Unreliable; fragment/parcel; ACK; reorder; retransmit.

> ⚠️ Trên `main` vẫn là model **single-dispatcher** (`incomplete_reliable_dispatcher`) — chỉ 1 reliable message bay cùng lúc. Bản parallel (map-based) nằm ở branch `dev/reliable-parallel`, chưa merge.

### Findings

**🟠 High — OOB `fragment_index` → remote crash (post-auth DoS) — ĐÃ XÁC NHẬN**
`src/delivery/receiver.c:280-282` (`add_fragment`) + `src/delivery/fragment.c:65-79` (decode)
`fragment_meta_decode` đọc `fragment_index`/`last_index` từ wire (mỗi cái tới 2 byte → 65535) **không kiểm `fragment_index <= last_index`**. `ensure_recv_command` cấp mảng `fragments` kích thước `last_index+1` (`receiver.c:119-120`); `add_fragment` gọi `array_get_ptr(fragments, fragment_index)` rồi `assert(fragment != NULL)` + deref — **không guard biên**. Một DATA fragment crafted (vd `last_index=1` → size 2, `fragment_index=1000`) → `get_ptr` trả NULL → `receiver.c:282` deref NULL → **crash** (release) / abort (debug).
- Fragment đi trên PAYLOAD đã mã hoá+HMAC → **post-auth**: một client đã kết nối crash được server bằng **1 gói**.
- **Đáng chú ý:** path ACK phía sender (`dispatcher.c` `recv_ack`) **đã guard đúng** (`if (!fragment) return;`) — receiver nên theo pattern đó.
- **Fix:** đổi `assert` thành `if (!fragment) return;` trong `add_fragment`; lý tưởng thêm kiểm `fragment_index <= last_index` lúc decode.

**🟡 Medium — Memory amplification do `last_index` không giới hạn**
`src/delivery/receiver.c:119-120` + `fragment.c:79`
`last_index` (tới 65535) định cỡ mảng `fragments` = `last_index+1` không cap. Peer đã kết nối gửi fragment với `last_index` lớn qua nhiều sequence → mỗi cái cấp tới 65536 slot fragment → cạn bộ nhớ.
- **Fix:** cap `last_index` theo số fragment tối đa hợp lý của một parcel; reject nếu vượt.

**🟡 Medium — Head-of-line blocking (design)**
`src/delivery/bus.c:525-537` — single `incomplete_reliable_dispatcher` → chỉ 1 reliable message bay cùng lúc; một reliable bị mất sẽ chặn mọi gói kế trên bus cho tới khi retransmit/timeout. (Parallel mode ở `dev/reliable-parallel`, chưa merge.)

### Điểm tốt (đã verify)
- **Reliable sequence đúng**: `++bus->sequence_generator` (`sender.c:304`) → sequence bắt đầu từ **1**; `last_recv_reliable_sequence` init 0 là sentinel an toàn → **không mất message đầu**; re-ACK fragment của parcel đã hoàn tất đúng (`bus.c:241`); ép 1 reliable bay cùng lúc (`bus.c:234-238`).
- **ACK path guard OOB đúng** (`dispatcher.c` `recv_ack`: `if (!fragment) return;`) — đây là pattern receiver nên theo.
- **Ref-count fragment cân bằng**: `attach_content` ref buffer, `cleanup` unref (`fragment.c:192-202`, `:14-21`).
- **Sequenced mode** drop gói cũ đúng (`bus.c:249`, `:306`).
- `ensure_recv_command` dùng pool acquire + cleanup xử lý map entry khi init fail → không dangling entry.

---

## 5. API Layer  ✅ (đã review + đã vá)

> **Trạng thái vá:** 🟡 invalid-channel leak + 🔵 duplicate decl đã vá trên branch `fix/api-hardening` (2 commit, chưa push). Verify độc lập: 2/2 API tests PASS. 🔵 plugin doc-note để follow-up. Xem `docs/superpowers/plans/2026-10-08-api-fixes.md`.

**Vai trò:** Socket/Session/Channel/Message public + plugin system + builtin impl.

### Findings

**🟡 Medium — `session_send` với channel không hợp lệ: leak message + kẹt BUSY + mất callback — ĐÃ XÁC NHẬN**
`src/api/session.c:51-62`
`pomelo_session_send` gọi `pomelo_message_prepare_send` (ref **+1** + cờ BUSY) ở đầu. Path `signature == 0` xử lý đúng (gọi `dispatch_send_result` trước khi return). Nhưng path `if (!channel) return;` (dòng 62) **return thẳng, không gọi `dispatch_send_result` cũng không `finish_send`**. Mà `dispatch_send_result` (`socket.c:583-606`) mới là nơi `finish_send` (unref -1 + clear BUSY) + gọi callback. Hậu quả khi truyền `channel_index` sai:
- ref +1 không được cân bằng → message **không bao giờ về pool** (leak).
- cờ BUSY kẹt → message không thể tái dùng.
- callback send **không bao giờ được gọi** → caller có thể treo.
- **Fix:** ở path `!channel`, gọi `pomelo_socket_dispatch_send_result(session->socket, message);` trước `return;` — y như path `signature == 0`.

**🔵 Low — Khai báo lặp `pomelo_message_write_uint8`**
`include/pomelo/api.h:533-534` — khai báo 2 lần (hợp lệ trong C, chỉ dư thừa → cleanup).

**🔵 Low — Plugin load/unload trong lúc dispatch callback → fail-fast**
`src/api/plugin/plugin.c:258-332` iterate `plugins` bằng iterator trong khi gọi callback plugin. `pomelo_list_iterator_next` (`list.c:497-503`) kiểm `mod_count`; nếu list bị sửa (plugin load/unload) giữa chừng → `assert` (debug) / trả -2 dừng sớm bỏ sót plugin (release). **Không corrupt memory** (fail-fast), nhưng là ràng buộc cần document: không load/unload plugin bên trong lifecycle callback.

### Điểm tốt (đã verify)
- **Ref-count message đúng**: `prepare_send` (ref+1, BUSY) ↔ `finish_send` (unref-1, clear BUSY); `on_finalize` → trả message về pool (`message.c:173-193`). Cân bằng trên happy-path và path signature==0 (chỉ lỗi ở path !channel ở trên).
- **Bounds đúng**: write kiểm `bytes+length > message_capacity` → OVERFLOW (`message.c:219`); read delegate reader → UNDERFLOW (`message.c:371-375`). Không over-read/over-write.
- **Stale-session guard thông minh**: `session_send` kiểm atomic `signature == 0` để phát hiện session đã release (caller giữ handle cũ) → dispatch failure thay vì UAF (`session.c:53-57`).
- **List iterator fail-fast** (`mod_count`) → sửa list giữa iteration bị bắt ngay, không corrupt âm thầm.
- Mọi thao tác chạy trên loop thread + sequencer tuần tự hoá → các nghi vấn "race" (signature-check vs send, v.v.) là **false positive**.

---

## 6. Bug phát hiện từ binding Node (pomelo-udp-node)  ✅ (N1 + N2 + N3 đã vá)

> Nguồn: test suite của `../pomelo-udp-node` (2026-10-08), submodule trỏ tới commit `f16bf3b` của repo này.
> Cả 2 đều tái hiện được qua binding Node; **chưa** có test C tương ứng ở đây. Phần "nguyên nhân" của N2 là giả thuyết từ backtrace, cần agent xác nhận.

### Findings

**✅ FIXED (High) — N1: `udp_bind` lỗi → abort ở `pomelo_platform_uv_udp_on_closed`**
`src/platform/uv/udp.c:153` + `:494`
- **Luồng lỗi:** `pomelo_platform_uv_udp_bind` acquire socket từ pool → `uv_udp_bind(...)` thất bại (vd. địa chỉ không thuộc máy → `EADDRNOTAVAIL`) → gọi `pomelo_platform_uv_udp_close(socket)` **trước khi** `socket->entry = pomelo_list_push_back(manager->sockets, socket)` (dòng ~170). Close callback `pomelo_platform_uv_udp_on_closed` gọi `pomelo_list_remove(manager->sockets, socket->entry)` với `entry == NULL` → `assert(entry != NULL)` (`src/utils/list.c:419`) → **SIGABRT**; build bỏ assert thì deref NULL.
- **Cùng pattern:** nhánh `uv_udp_recv_start` lỗi (dòng ~163) cũng close trước khi push. Nên kiểm tra luôn `pomelo_platform_uv_udp_connect` (dòng ~224) và các nhánh lỗi tương tự.
- **Repro (qua binding):** `socket.listen(key, 1, 4, "203.0.113.1:19999")` → promise reject "Failed to listen" → vòng loop kế tiếp abort. Backtrace: `uv_run → pomelo_platform_uv_udp_on_closed (udp.c:494) → pomelo_list_remove(entry=NULL)`.
- **Hướng fix:** trong `on_closed` chỉ `pomelo_list_remove` khi `socket->entry != NULL` (và set lại `entry = NULL` sau khi remove); hoặc push vào list trước khi có thể close. Kiểm tra thêm `pomelo_platform_uv_udp_mgr_check_closed` không bị gọi sai khi socket chưa từng nằm trong list. Thêm regression test bind vào địa chỉ không bind được.
- **✅ Đã vá:** `pomelo_platform_uv_udp_on_closed` giờ chỉ `pomelo_list_remove` khi `socket->entry != NULL` rồi set `entry = NULL` (socket pool là `zero_init` nên `entry` chắc chắn NULL ở đường lỗi bind/connect). Fix nằm ở `on_closed` — sink chung của cả 3 call-site (bind/`uv_udp_bind`, bind/`uv_udp_recv_start`, connect). Regression test C: `test/platform-test/platform-bind-fail-test.c` (`pomelo-test-platform-bind-fail`, đã vào CTest) — bind `203.0.113.1:19999` (TEST-NET-3 → `EADDRNOTAVAIL`): **đỏ trước fix** (abort `list.c:419`, exit 134), **xanh sau fix** (5/5, no leak). Full CTest 20/20 PASS.

**✅ FIXED (High) — N2: thứ tự huỷ trong `pomelo_context_root_destroy` → "Try to ref finalized reference"**
`src/api/context.c:231` (`pomelo_context_root_destroy`)
- **Triệu chứng:** `Assertion failed: (false && "Try to ref finalized reference"), function pomelo_reference_ref, file ref.c, line 23` khi huỷ context lúc vẫn còn session đang có message gửi dở.
- **Thứ tự hiện tại:** `socket_pool` → **`message_pool`** → `builtin_session_pool` → `builtin_channel_pool` → `plugin_*_pool` → protocol/delivery/buffer context → plugin manager.
- **Giả thuyết (từ backtrace của binding):** cleanup của session/channel builtin (khi destroy `builtin_session_pool`) huỷ các transmission/parcel còn treo → đi tới `pomelo_socket_dispatch_send_result` (vd. `src/api/builtin/delivery.c:97` hoặc `src/api/builtin/channel.c:115/125`) → `pomelo_message_ref` trên message mà `message_pool` đã finalize/free trước đó.
- **Repro (qua binding, không ổn định ~2/6 lần):** trong `worker_thread`, mở server + client, gửi message liên tục mỗi 1 ms, rồi `worker.terminate()` từ main thread. Env teardown → binding gọi `pomelo_context_destroy` khi socket vẫn đang chạy.
- **Hướng fix (cần agent xác nhận):** huỷ `message_pool` **sau** các pool session/channel (và sau delivery context nếu parcel giữ message); hoặc đảm bảo destroy socket/session flush send-result trước. Viết test C: context có session đang gửi (reliable, chưa ACK) → `pomelo_context_destroy` không assert, không leak.
- **Lưu ý:** binding Node đã có async cleanup hook ở platform-uv để đóng handle khi teardown, nhưng **không** thể chờ socket stop bất đồng bộ trước khi destroy context → cần native chịu được destroy khi còn session.
- **✅ Đã xác nhận + vá:** đúng là chuỗi `builtin_session_pool` destroy → `pomelo_session_builtin_cleanup` → `endpoint_stop` (chạy **đồng bộ** qua sequencer) → `bus_stop` → `dispatcher_cancel` → `sender_complete` → `pomelo_delivery_sender_on_result` → `pomelo_socket_dispatch_send_result` → `pomelo_message_ref()` trên message **đã bị `message_pool` free** (và deref socket đã bị `socket_pool` free). Fix: **đảo thứ tự huỷ** trong `pomelo_context_root_destroy` — huỷ session pool (builtin+plugin) → channel pool → rồi mới `socket_pool`/`message_pool` → protocol/delivery/buffer context → plugin manager. Nhờ vậy lúc session flush các send dở thì socket + message còn sống (vá luôn cả hazard socket-use-after-free). Session cleanup giải phóng channel về channel pool nên session phải huỷ trước channel.
- **Repro C (deterministic, đã vào CTest):** `test/api-test/api-destroy-inflight-test.c` (`pomelo-test-api-destroy-inflight`) — connect client↔server loopback, gửi 1 message RELIABLE rồi `uv_stop` (ACK chưa về → in-flight), gọi thẳng `pomelo_context_destroy` không stop socket. **Đỏ trước fix 6/6** (abort `ref.c:23`, exit 134), **xanh sau fix 8/8** (no abort, no leak). Full CTest 21/21 PASS.

**✅ FIXED (Medium) — N3: server bind với `UV_UDP_REUSEADDR` → bind trùng port không báo lỗi**
`src/platform/uv/udp.c:153` (`pomelo_platform_uv_udp_bind`)
- **Triệu chứng:** 2 process (hoặc 2 socket) `listen` cùng `ip:port` đều thành công; kernel chỉ giao datagram unicast cho một socket → server kia "câm", client connect/gửi treo. Phát hiện khi test suite của binding Node bị treo do đụng port với process chạy song song (2026-10-09). Tái hiện qua binding: 2 process cùng `listen("127.0.0.1:19950")` → cả 2 resolve.
- **Nguyên nhân:** libuv map `UV_UDP_REUSEADDR` thành `SO_REUSEPORT` (BSD/macOS) / `SO_REUSEADDR` (Linux, Windows). Trên Windows còn tệ hơn: `SO_REUSEADDR` cho phép bind đè cả socket không set cờ (port hijack). Cờ này không cần cho restart nhanh: UDP không có `TIME_WAIT` — đã kiểm chứng trên macOS: rebind ngay sau khi process cũ thoát / bị `kill -9` / sau `close()` trong cùng process đều OK; chỉ fail khi process cũ còn sống (đúng hành vi mong muốn). Linux/Windows: suy từ tài liệu, chưa chạy thực tế.
- **✅ Đã vá:** bind server với flags `0`. Bind trùng → `EADDRINUSE` → `pomelo_platform_udp_bind` trả lỗi → `pomelo_socket_listen` thất bại. Nếu sau này cần nhiều process share 1 port (load-balance) thì thêm option riêng dùng `UV_UDP_REUSEPORT`, mặc định tắt.
- **Regression test:** thêm `pomelo_test_platform_bind_in_use` vào `test/platform-test/platform-bind-fail-test.c`: bind `127.0.0.1:19998` → bind lần 2 phải fail → `udp_stop` → bind lại ngay phải OK → no leak. **Đỏ trước fix** (fail `platform-bind-fail-test.c:81`), **xanh sau fix**. Full CTest 21/21 PASS.

---

## Mô hình nền (ghi chú chung — xác nhận khi review platform)
- **Bộ nhớ:** atomic ref-counting lock-free (CAS, `src/base/ref.c`) + object pool (`src/utils/pool.c`) + allocator abstraction.
- **Luồng:** mỗi context 1 loop thread; `sequencer` (`src/base/sequencer.c`) tuần tự hoá task → đa số logic không chạy song song. Crypto offload sang libuv threadpool (worker), trả kết quả về loop qua completion callback. Atomic ref-count cần vì buffer/parcel/message được ref xuyên biên worker↔loop.
