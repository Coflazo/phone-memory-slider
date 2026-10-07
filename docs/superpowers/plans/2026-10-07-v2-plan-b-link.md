# Plan B: QR + Bluetooth link and the Android companion

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace v1's "QR secret as a bearer token over HTTP-over-RFCOMM" with an authenticated, encrypted, resumable link: QR pairing, `Noise_IK_25519_ChaChaPoly_SHA256`, a compact binary protocol, a desktop RFCOMM server, and a rewritten Android companion that streams its catalog, previews and keyframes and moves chosen items to Android's system Trash.

**Architecture:** Protocol, framing, QR payload and the Noise session are pure C++ in `core/link/` (vendored `noise-c`, MIT) so they are unit-tested and fuzzed without Bluetooth. The desktop owns a `ByteStream` interface with two implementations: an RFCOMM server (Qt Bluetooth, worker thread) and an in-memory pipe for tests. The Android app mirrors the protocol in Kotlin with `org.signal.forks:noise-java`; both sides prove interop with golden transcripts generated from fixed keys.

**Tech Stack:** C++23, noise-c (signalapp fork, MIT), Qt 6.8.3 Bluetooth, Kotlin 2.x, Jetpack Compose, CameraX, `io.github.zxing-cpp:android`, `org.signal.forks:noise-java:0.1.1` (MIT), libFuzzer (clang, Linux CI).

**Spec:** `C:\Users\Cagan\.claude\plans\okey-i-want-you-dapper-thacker.md` ("Phone link: QR + Bluetooth" and "Security") and `docs/spikes.md`. Spike 2 (insecure RFCOMM against Windows) is still pending; Task 5 carries both outcomes.

## Global Constraints

- No IP networking anywhere: no sockets, no INTERNET permission on Android (checked on the merged manifest), no HTTP. Bluetooth RFCOMM only; BLE is a documented future transport.
- Handshake pattern exactly `Noise_IK_25519_ChaChaPoly_SHA256`, phone = initiator, prologue = ASCII `PMS/1` + the 16 service-UUID bytes.
- QR content exactly `PMS1:` + RFC 4648 Base32 (uppercase, no padding) of: `u8 version=1 | 6-byte Bluetooth MAC | 16-byte service UUID | 32-byte desktop static X25519 public key | 16-byte one-time token | u32 BE expiry unix seconds` (75 bytes -> 120 chars). Token lifetime 120 s, single use.
- Frames: `u16 BE length | bytes`, length <= 65535. Before the handshake completes, the responder accepts one frame of at most 512 bytes and never allocates from an unvalidated length.
- Plaintext messages: `u8 type | u32 BE request_id | payload`; strings are `u16 BE length | UTF-8`, max 1024 bytes; integers big-endian. Unknown type or malformed payload closes the session.
- At most 16 requests in flight per session; previews are JPEG, long side 256 px, <= 60 KB.
- Listen only while the Connect or Sync screen is open; one session at a time; handshake timeout 5 s; at most 1 handshake attempt per second; session closes after 5 min idle.
- Desktop stores pinned phones (static key, model, pair time) in the platform key store (DPAPI-protected file on Windows, Keychain on macOS, libsecret on Linux); the phone stores the pinned desktop key in Android Keystore-wrapped storage.
- Trash requests are bound to `(id, size, date_modified, sha256)`; the phone re-validates, never trashes favorites, and uses `MediaStore.createTrashRequest` so Android's own dialog is the final gate; chunks of <= 500.
- Commits as Coflazo, no AI trailers.

## Review Focus

1. Someone photographs the QR and races the real phone: only the first valid handshake with the token wins; the second sees a refusal, and the desktop shows "Paired with <model> · Not you? Unpair". Pinned in Task 3.
2. A phone reconnects after the desktop restarted: the pinned key is accepted without a new QR; an unpinned key without a fresh token is dropped. Pinned in Task 3.
3. The phone's gallery changes during a transfer (photos added/deleted): the next `CATALOG_SINCE` returns adds and removals; previews for removed ids return `Missing`, never abort the run. Pinned in Tasks 4 and 7.
4. The user rotates the phone or switches apps mid-transfer: the foreground service keeps the session; the UI reattaches. Pinned in Task 7.
5. A flood of garbage bytes on the RFCOMM socket: the desktop drops that connection within 5 s, allocates nothing large, and keeps listening. Pinned in Tasks 1 and 5.

---

## File structure

```
third_party/noise-c/                         vendored noise-c (signalapp fork, MIT), only protocol + ref backend sources
core/include/pms/link/frames.hpp   core/src/link/frames.cpp      u16 framing, incremental decoder with caps
core/include/pms/link/messages.hpp core/src/link/messages.cpp    typed messages <-> bytes
core/include/pms/link/qr.hpp       core/src/link/qr.cpp          Base32 + QR payload
core/include/pms/link/session.hpp  core/src/link/session.cpp     Noise IK initiator/responder, token, pinning, SAS
core/src/link/random.cpp                                         noise_rand_bytes via OS CSPRNG
core/tests/link_*_tests.cpp   core/tests/fixtures/cacophony_ik.txt   core/tests/fixtures/golden_*.bin
fuzz/                                                            libFuzzer targets (Linux clang CI)
desktop/src/link/byte_stream.hpp   desktop/src/link/rfcomm_server.{hpp,cpp}   desktop/src/link/pipe_stream.{hpp,cpp}
desktop/src/link/companion_client.{hpp,cpp}                      request/response over a Session (catalog, previews, keyframes, hashes, trash)
android/ (renamed from android-companion/)                       rewritten app
docs/PROTOCOL.md
```

---

### Task 1: Frames and messages

**Files:** Create `core/include/pms/link/frames.hpp`, `core/src/link/frames.cpp`, `core/include/pms/link/messages.hpp`, `core/src/link/messages.cpp`; Test `core/tests/link_frames_tests.cpp`, `core/tests/link_messages_tests.cpp`; Create `docs/PROTOCOL.md` (message table below, written for a third-party implementer)

**Interfaces:**
```cpp
namespace pms::link {
inline constexpr std::size_t max_frame = 65535;
inline constexpr std::size_t max_preauth_frame = 512;
[[nodiscard]] std::vector<std::byte> encode_frame(std::span<const std::byte> payload);  // throws std::length_error if > max_frame
class FrameDecoder {
public:
    explicit FrameDecoder(std::size_t max_payload) noexcept;
    void set_max_payload(std::size_t max_payload) noexcept;
    // Appends bytes; returns false (and stays failed) on a length above the cap. Never buffers more than cap + 2 bytes.
    [[nodiscard]] bool feed(std::span<const std::byte> bytes);
    [[nodiscard]] std::optional<std::vector<std::byte>> next();
    [[nodiscard]] bool failed() const noexcept;
};
enum class MsgType : std::uint8_t { Hello = 0x01, HelloAck = 0x02, CatalogSince = 0x10, CatalogPage = 0x11,
    PreviewRequest = 0x12, Preview = 0x13, KeyframesRequest = 0x14, Keyframes = 0x15, HashRequest = 0x16, Hash = 0x17,
    TrashRequest = 0x20, TrashResult = 0x21, Error = 0x30, Bye = 0x31 };
enum class ItemStatus : std::uint8_t { Ok = 0, Missing = 1, Unreadable = 2, Busy = 3 };
enum class TrashOutcome : std::uint8_t { Trashed = 0, UserDeclined = 1, Changed = 2, Missing = 3, Favorite = 4, Failed = 5 };
struct Hello { std::uint16_t protocol{1}; std::string model; std::string os; std::string app_version; std::uint32_t capabilities{}; };
struct HelloAck { std::uint16_t protocol{1}; std::string desktop_name; };
struct CatalogSince { std::string store_version; std::uint64_t generation{}; std::uint32_t max_items{2000}; };
struct CatalogItem { std::uint64_t id{}; std::uint8_t media_type{}; std::string relative_path; std::string mime;
    std::uint64_t size{}; std::uint32_t width{}, height{}, duration_ms{}; std::int64_t date_taken_ms{}, date_modified_s{};
    std::uint64_t generation_modified{}; bool favorite{}; };
struct CatalogPage { std::string store_version; std::uint64_t next_generation{}; bool full_resync{}; bool more{};
    std::vector<CatalogItem> items; std::vector<std::uint64_t> removed; };
struct IdList { std::vector<std::uint64_t> ids; };                                  // PreviewRequest, HashRequest (<= 16 ids)
struct Preview { std::uint64_t id{}; ItemStatus status{}; std::uint32_t width{}, height{}; std::vector<std::byte> jpeg; };
struct KeyframesRequest { std::uint64_t id{}; std::uint8_t count{8}; };
struct Keyframes { std::uint64_t id{}; ItemStatus status{}; std::vector<std::vector<std::byte>> jpegs; };
struct Hash { std::uint64_t id{}; ItemStatus status{}; std::array<std::uint8_t, 32> sha256{}; };
struct TrashItem { std::uint64_t id{}; std::uint64_t size{}; std::int64_t date_modified_s{}; std::array<std::uint8_t, 32> sha256{}; };
struct TrashRequest { std::vector<TrashItem> items; };                              // <= 500
struct TrashResult { std::vector<std::pair<std::uint64_t, TrashOutcome>> items; };
struct ErrorMsg { std::uint16_t code{}; std::string message; };
using Body = std::variant<Hello, HelloAck, CatalogSince, CatalogPage, IdList, Preview, KeyframesRequest, Keyframes, Hash,
                          TrashRequest, TrashResult, ErrorMsg, std::monostate /*Bye*/>;
struct Message { MsgType type{}; std::uint32_t request_id{}; Body body; };
[[nodiscard]] std::vector<std::byte> encode(const Message& m);           // throws std::length_error over 65535 - 16 bytes
[[nodiscard]] std::optional<Message> decode(std::span<const std::byte> plaintext) noexcept;  // nullopt on any violation
}
```
Caps enforced in `decode`: strings <= 1024 bytes, catalog items <= 2000 per page, removed ids <= 20000, id lists <= 16, keyframes <= 8 of <= 60 KB each, preview <= 60 KB, trash items <= 500; trailing bytes are a violation.

- [ ] **Step 1: Failing tests**

```cpp
// core/tests/link_frames_tests.cpp
#include <vector>
#include "pms/link/frames.hpp"
#include "test_harness.hpp"
void run_link_frames_tests() {
    using namespace pms::link;
    std::vector<std::byte> payload(300, std::byte{7});
    const auto f = encode_frame(payload);
    pms_check(f.size() == 302 && f[0] == std::byte{0x01} && f[1] == std::byte{0x2C}, "u16 BE length prefix");
    FrameDecoder d(max_frame);
    pms_check(d.feed(std::span(f).subspan(0, 1)) && !d.next(), "partial header waits");
    pms_check(d.feed(std::span(f).subspan(1)), "rest feeds");
    const auto got = d.next();
    pms_check(got && *got == payload, "frame reassembled");
    FrameDecoder pre(max_preauth_frame);
    const std::vector<std::byte> huge{std::byte{0xFF}, std::byte{0xFF}};
    pms_check(!pre.feed(huge) && pre.failed(), "pre-auth frames above 512 bytes fail without buffering");
    bool threw = false;
    try { (void)encode_frame(std::vector<std::byte>(70000)); } catch (const std::length_error&) { threw = true; }
    pms_check(threw, "oversized frames are refused at encode");
}
```
```cpp
// core/tests/link_messages_tests.cpp
#include <vector>
#include "pms/link/messages.hpp"
#include "test_harness.hpp"
void run_link_messages_tests() {
    using namespace pms::link;
    CatalogPage page{"v1", 42, false, true, {}, {7, 8}};
    page.items.push_back({1, 1, "DCIM/Camera/a.jpg", "image/jpeg", 123456, 4000, 3000, 0, 1700000000000, 1700000000, 41, true});
    const Message m{MsgType::CatalogPage, 9, page};
    const auto bytes = encode(m);
    const auto back = decode(bytes);
    pms_check(back && back->request_id == 9, "round trip keeps request id");
    const auto& p = std::get<CatalogPage>(back->body);
    pms_check(p.items.size() == 1 && p.items[0].favorite && p.items[0].relative_path == "DCIM/Camera/a.jpg" && p.removed.size() == 2, "fields survive");
    auto trailing = bytes; trailing.push_back(std::byte{0});
    pms_check(!decode(trailing), "trailing bytes rejected");
    auto cut = bytes; cut.resize(cut.size() - 3);
    pms_check(!decode(cut), "truncated message rejected");
    std::vector<std::byte> unknown{std::byte{0x77}, std::byte{0}, std::byte{0}, std::byte{0}, std::byte{1}};
    pms_check(!decode(unknown), "unknown type rejected");
    IdList many; many.ids.resize(17);
    bool threw = false;
    try { (void)encode({MsgType::PreviewRequest, 1, many}); } catch (const std::length_error&) { threw = true; }
    pms_check(threw, "id lists above 16 are refused");
    pms_check(decode(encode({MsgType::Bye, 0, std::monostate{}})).has_value(), "Bye has no payload");
}
```
- [ ] **Step 2: run, expect compile failure. Step 3: implement** a `Writer` (`u8/u16/u32/u64/i64/str/bytes`) and a `Reader` that returns `std::nullopt` on any overrun and tracks `remaining()`; one encode/decode function per body type keyed by `MsgType`. **Step 4: run, expect pass.**
- [ ] **Step 5: Write `docs/PROTOCOL.md`** (handshake, QR, frames, every message with byte layout and caps, flow control, resume, trash binding, error codes) and **commit** `feat(link): length-capped frames and a strict binary protocol`.

---

### Task 2: QR payload and Base32

**Files:** Create `core/include/pms/link/qr.hpp`, `core/src/link/qr.cpp`; Test `core/tests/link_qr_tests.cpp`

**Interfaces:**
```cpp
namespace pms::link {
struct PairingCode { std::array<std::uint8_t, 6> mac{}; std::array<std::uint8_t, 16> service_uuid{};
                     std::array<std::uint8_t, 32> desktop_key{}; std::array<std::uint8_t, 16> token{}; std::uint32_t expiry_unix_s{}; };
[[nodiscard]] std::string base32_encode(std::span<const std::uint8_t> bytes);           // RFC 4648, uppercase, no padding
[[nodiscard]] std::optional<std::vector<std::uint8_t>> base32_decode(std::string_view text) noexcept;
[[nodiscard]] std::string encode_pairing(const PairingCode& code);                       // "PMS1:" + 120 chars
[[nodiscard]] std::optional<PairingCode> decode_pairing(std::string_view text, std::uint32_t now_unix_s) noexcept;  // nullopt if malformed or expired
}
```
- [ ] **Step 1: Failing test**

```cpp
#include "pms/link/qr.hpp"
#include "test_harness.hpp"
void run_link_qr_tests() {
    using namespace pms::link;
    const std::vector<std::uint8_t> foo{'f', 'o', 'o', 'b', 'a', 'r'};
    pms_check(base32_encode(foo) == "MZXW6YTBOI", "RFC 4648 vector foobar");
    pms_check(base32_decode("MZXW6YTBOI") == foo, "decode vector");
    pms_check(!base32_decode("MZXW6YTB0I"), "digit zero is not Base32");
    PairingCode c; c.mac = {1, 2, 3, 4, 5, 6}; c.token[0] = 9; c.desktop_key[31] = 0xAB; c.expiry_unix_s = 1000;
    const auto text = encode_pairing(c);
    pms_check(text.size() == 125 && text.starts_with("PMS1:"), "prefix + 120 Base32 chars");
    for (char ch : text) pms_check((ch >= 'A' && ch <= 'Z') || (ch >= '2' && ch <= '7') || ch == ':' || ch == 'P' || ch == 'M' || ch == 'S' || ch == '1', "QR alphanumeric charset only");
    const auto back = decode_pairing(text, 999);
    pms_check(back && back->mac == c.mac && back->desktop_key == c.desktop_key && back->token == c.token, "round trip");
    pms_check(!decode_pairing(text, 1001), "expired codes are refused");
    pms_check(!decode_pairing("PMS2:" + text.substr(5), 999), "unknown prefix refused");
    pms_check(!decode_pairing(text.substr(0, 60), 999), "truncated code refused");
}
```
- [ ] **Steps 2-5:** fail -> implement -> pass -> commit `feat(link): QR pairing payload in QR alphanumeric mode`.

---

### Task 3: Noise IK session with token, pinning and SAS

**Files:**
- Create: `third_party/noise-c/` (from `https://github.com/signalapp/noise-c` at a pinned commit recorded in its README: `include/noise/`, `src/protocol/*.c`, `src/crypto/{chacha,curve25519/donna,sha2,blake2}`, `src/backend/ref/*.c`, LICENSE), a small `third_party/noise-c/CMakeLists.txt` building static `noise_c` without `/WX`
- Create: `core/src/link/random.cpp` (provides `noise_rand_bytes`: `BCryptGenRandom` on Windows, `getentropy` on Linux/macOS)
- Create: `core/include/pms/link/session.hpp`, `core/src/link/session.cpp`
- Create: `core/tests/fixtures/cacophony_ik.txt` (the `Noise_IK_25519_ChaChaPoly_SHA256` entries from `https://raw.githubusercontent.com/haskell-cryptography/cacophony/master/vectors/cacophony.txt`, saved verbatim with source URL and date in a header comment), `core/tests/link_session_tests.cpp`, `core/tests/fixtures/golden_ik_transcript.bin` (written by the test in `--write-golden` mode; consumed by Android in Task 7)

**Interfaces:**
```cpp
namespace pms::link {
using Key32 = std::array<std::uint8_t, 32>;
struct KeyPair { Key32 public_key{}; Key32 private_key{}; };
[[nodiscard]] KeyPair generate_keypair();
struct Pinned { Key32 phone_key{}; std::string model; std::int64_t paired_unix_s{}; };
class PinStore { public: virtual ~PinStore() = default;
    [[nodiscard]] virtual std::optional<Pinned> find(const Key32& phone_key) const = 0;
    virtual void pin(const Pinned& p) = 0; virtual void unpin(const Key32& phone_key) = 0; };
enum class HandshakeResult : std::uint8_t { Continue, Established, Rejected };
class ResponderSession {   // desktop
public:
    ResponderSession(KeyPair desktop, std::array<std::uint8_t, 16> service_uuid, PinStore& pins);
    void open_pairing_window(const std::array<std::uint8_t, 16>& token, std::uint32_t expiry_unix_s);
    // msg1 plaintext payload = u8 kind (1 = first pairing with token, 2 = reconnect) | 16-byte token (kind 1) | string model.
    [[nodiscard]] HandshakeResult on_message1(std::span<const std::byte> msg1, std::uint32_t now_unix_s, std::vector<std::byte>& msg2_out);
    [[nodiscard]] std::string sas() const;              // 6 digits from the handshake hash, after Established
    [[nodiscard]] const Pinned& peer() const;
    [[nodiscard]] std::vector<std::byte> seal(std::span<const std::byte> plaintext);           // transport encrypt
    [[nodiscard]] std::optional<std::vector<std::byte>> open(std::span<const std::byte> ct);  // nullopt = close session
};
class InitiatorSession {   // phone side, used by tests and the golden transcript; Kotlin mirrors it
public:
    InitiatorSession(KeyPair phone, Key32 desktop_static, std::array<std::uint8_t, 16> service_uuid);
    [[nodiscard]] std::vector<std::byte> message1_first_pairing(const std::array<std::uint8_t, 16>& token, std::string_view model);
    [[nodiscard]] std::vector<std::byte> message1_reconnect(std::string_view model);
    [[nodiscard]] bool on_message2(std::span<const std::byte> msg2);
    [[nodiscard]] std::string sas() const;
    [[nodiscard]] std::vector<std::byte> seal(std::span<const std::byte> plaintext);
    [[nodiscard]] std::optional<std::vector<std::byte>> open(std::span<const std::byte> ct);
};
// Test hook: fixed ephemeral keys for golden transcripts (never used by the app).
void set_fixed_ephemeral_for_tests(std::optional<KeyPair> responder, std::optional<KeyPair> initiator);
}
```
SAS = `(u32 BE of the first 4 bytes of the handshake hash) mod 1,000,000`, zero-padded to 6 digits. On `Established` with kind 1 the token is burned (`open_pairing_window` must be called again for another pairing) and the phone key is pinned; kind 2 requires an existing pin.

- [ ] **Step 1: Failing test**

```cpp
#include <fstream>
#include <map>
#include "pms/link/session.hpp"
#include "test_harness.hpp"
namespace {
struct MemoryPins : pms::link::PinStore {
    std::map<pms::link::Key32, pms::link::Pinned> m;
    std::optional<pms::link::Pinned> find(const pms::link::Key32& k) const override { auto it = m.find(k); return it == m.end() ? std::nullopt : std::optional(it->second); }
    void pin(const pms::link::Pinned& p) override { m[p.phone_key] = p; }
    void unpin(const pms::link::Key32& k) override { m.erase(k); }
};
}  // namespace
void run_link_session_tests() {
    using namespace pms::link;
    // 1. cacophony vectors for the exact pattern (parse init/resp static+ephemeral, prologue, messages; compare ciphertexts).
    pms_check(run_cacophony_ik_vectors(std::string(PMS_TEST_FIXTURES) + "/cacophony_ik.txt") >= 1, "cacophony IK vectors pass");
    const std::array<std::uint8_t, 16> uuid{1, 2, 3};
    const auto desk = generate_keypair(), phone = generate_keypair(), thief = generate_keypair();
    MemoryPins pins;
    ResponderSession r(desk, uuid, pins);
    const std::array<std::uint8_t, 16> token{42};
    r.open_pairing_window(token, 1000);
    InitiatorSession i(phone, desk.public_key, uuid);
    std::vector<std::byte> m2;
    pms_check(r.on_message1(i.message1_first_pairing(token, "Pixel 9"), 900, m2) == HandshakeResult::Established, "first pairing with the QR token");
    pms_check(i.on_message2(m2) && i.sas() == r.sas() && r.sas().size() == 6, "both sides derive the same 6-digit code");
    pms_check(pins.find(phone.public_key).has_value(), "phone key pinned");
    // Review Focus 1: the token is single use.
    ResponderSession r2(desk, uuid, pins);
    InitiatorSession t(thief, desk.public_key, uuid);
    pms_check(r2.on_message1(t.message1_first_pairing(token, "Thief"), 901, m2) == HandshakeResult::Rejected, "a reused token is rejected");
    // Expired window.
    ResponderSession r3(desk, uuid, pins);
    r3.open_pairing_window(token, 1000);
    pms_check(r3.on_message1(InitiatorSession(thief, desk.public_key, uuid).message1_first_pairing(token, "x"), 1001, m2) == HandshakeResult::Rejected, "expired window rejects");
    // Review Focus 2: reconnect with the pin, no token.
    ResponderSession r4(desk, uuid, pins);
    InitiatorSession again(phone, desk.public_key, uuid);
    pms_check(r4.on_message1(again.message1_reconnect("Pixel 9"), 5000, m2) == HandshakeResult::Established && again.on_message2(m2), "pinned phone reconnects");
    ResponderSession r5(desk, uuid, pins);
    pms_check(r5.on_message1(InitiatorSession(thief, desk.public_key, uuid).message1_reconnect("x"), 5000, m2) == HandshakeResult::Rejected, "unpinned reconnect rejected");
    // Wrong desktop key (phone scanned someone else's QR) fails closed.
    InitiatorSession wrong(phone, thief.public_key, uuid);
    ResponderSession r6(desk, uuid, pins);
    pms_check(r6.on_message1(wrong.message1_reconnect("x"), 5000, m2) == HandshakeResult::Rejected, "msg1 for another desktop is rejected");
    // Transport: tamper closes.
    auto ct = again.seal(std::vector<std::byte>{std::byte{1}, std::byte{2}});
    auto ok = r4.open(ct);
    pms_check(ok && ok->size() == 2, "transport round trip");
    auto ct2 = again.seal(std::vector<std::byte>{std::byte{3}});
    ct2[0] ^= std::byte{1};
    pms_check(!r4.open(ct2), "tampered ciphertext closes the session");
    // Garbage msg1 is rejected without crashing.
    std::vector<std::byte> junk(200, std::byte{0x5A});
    ResponderSession r7(desk, uuid, pins);
    pms_check(r7.on_message1(junk, 5000, m2) == HandshakeResult::Rejected, "garbage msg1 rejected");
}
```
`run_cacophony_ik_vectors(path)` lives in the test file: parses the JSON-lines vectors for the pattern, sets fixed keys via `set_fixed_ephemeral_for_tests`, runs messages, compares bytes, returns the count checked.
- [ ] **Step 2: run, expect failure. Step 3: implement** over noise-c's `NoiseHandshakeState` (`noise_handshakestate_new_by_name("Noise_IK_25519_ChaChaPoly_SHA256", ...)`, `set_prologue`, local/remote keypairs, `write_message`/`read_message`, `split`), constant-time token compare, pin store calls, SAS from `noise_handshakestate_get_handshake_hash`. **Step 4: pass**, then run once with `--write-golden` to produce `golden_ik_transcript.bin` (fixed static and ephemeral keys, token, model, msg1, msg2, two transport messages each way) and commit it.
- [ ] **Step 5: Commit** `feat(link): Noise IK session with single-use QR token, pinning and SAS`.

---

### Task 4: Companion client protocol logic (desktop, transport-agnostic)

**Files:** Create `desktop/src/link/byte_stream.hpp`, `desktop/src/link/pipe_stream.{hpp,cpp}`, `desktop/src/link/companion_client.{hpp,cpp}`, `desktop/tests/companion_client_tests.cpp` (QtTest, existing pattern in `desktop/tests/`), `desktop/tests/fake_phone.{hpp,cpp}` (a scripted phone built on `InitiatorSession` + `messages.hpp`)

**Interfaces:**
```cpp
namespace pms::desktop {
class ByteStream : public QObject { Q_OBJECT
public:
    virtual void write(QByteArray bytes) = 0; virtual void close() = 0;
signals: void bytesReceived(QByteArray bytes); void closed(QString reason); };
class PipeStream;  // two connected in-memory ends: static std::pair<PipeStream*, PipeStream*> create(QObject* parent)
class CompanionClient : public QObject { Q_OBJECT
public:
    CompanionClient(ByteStream* stream, pms::link::ResponderSession* session, QObject* parent = nullptr);
    void requestCatalog(QString storeVersion, quint64 generation);
    void requestPreviews(QList<quint64> ids);          // queued; at most 16 in flight; re-requests missing on reconnect
    void requestKeyframes(quint64 id, int count);
    void requestHashes(QList<quint64> ids);
    void requestTrash(QList<pms::link::TrashItem> items);   // chunks of 500
signals:
    void helloReceived(pms::link::Hello hello);
    void catalogPage(pms::link::CatalogPage page);
    void preview(pms::link::Preview preview);
    void keyframes(pms::link::Keyframes frames);
    void hash(pms::link::Hash hash);
    void trashResult(pms::link::TrashResult result);
    void failed(QString reason);                       // protocol violation or closed stream; never on per-item Missing
};
}
```
- [ ] **Step 1: Failing tests** (QtTest): fake phone serves a 2,500-item catalog in two pages -> both pages arrive, `more=false` on the last; 40 preview ids -> never more than 16 outstanding (fake phone records the max), all 40 answered; one id deleted on the phone mid-run -> its preview arrives with `ItemStatus::Missing` and `failed` is not emitted (Review Focus 3); 1,200 trash items -> three `TrashRequest` messages of 500/500/200 and three results; fake phone sends a 2 KB string field -> `failed` emitted and the stream closed.
- [ ] **Steps 2-5:** fail -> implement (request-id map, in-flight window, queue) -> pass -> commit `feat(desktop): companion protocol client with windowed requests`.

---

### Task 5: Desktop RFCOMM server

**Files:** Create `desktop/src/link/rfcomm_server.{hpp,cpp}`, `desktop/src/link/pin_store.{hpp,cpp}` (DPAPI file on Windows; Keychain/libsecret behind the same interface on macOS/Linux, with a 0600 file fallback only when no secret service exists, logged); Test `desktop/tests/rfcomm_server_tests.cpp` (guard logic only; real radio in S9)

**Interfaces:**
```cpp
namespace pms::desktop {
class RfcommServer : public QObject { Q_OBJECT
public:
    explicit RfcommServer(QObject* parent = nullptr);
    // Registers the SDP record and listens; returns the local adapter MAC for the QR. Listening stops on stop().
    [[nodiscard]] std::optional<std::array<std::uint8_t, 6>> start(const QBluetoothUuid& service);
    void stop();
signals:
    void sessionStarted(ByteStream* stream);   // exactly one live session; further connections are refused
    void rejected(QString reason);             // timeout, garbage, rate limit
};
// Pure guard used by the server and unit-tested without Bluetooth.
class ConnectionGuard { public:
    [[nodiscard]] bool allow_attempt(std::chrono::steady_clock::time_point now);   // <= 1 per second
    [[nodiscard]] bool handshake_expired(std::chrono::steady_clock::time_point started, std::chrono::steady_clock::time_point now) const; // 5 s
    [[nodiscard]] bool idle_expired(std::chrono::steady_clock::time_point last, std::chrono::steady_clock::time_point now) const;       // 5 min
};
}
```
Implementation notes: Qt's WinRT `QBluetoothSocket::write` blocks the caller, so each accepted socket lives on a dedicated `QThread` and talks to the GUI thread through queued signals; the pre-auth decoder is a `FrameDecoder(max_preauth_frame)` (Review Focus 5). If spike 2 shows Windows refuses unpaired insecure connections, add the guided OS pairing step to onboarding (Plan C) and keep this server unchanged; if Qt's server is flaky in spike 2, replace the internals with a Winsock `AF_BTH` + `WSASetService` server behind the same class.

- [ ] **Step 1: Failing test** for `ConnectionGuard` (second attempt within 1 s refused; handshake at 5.1 s expired; idle at 5 min expired) and for the "one live session" rule via a test seam that injects `PipeStream` ends instead of sockets.
- [ ] **Steps 2-5:** implement -> pass -> commit `feat(desktop): RFCOMM server with one session and handshake guards`.

---

### Task 6: Fuzz targets

**Files:** Create `fuzz/CMakeLists.txt`, `fuzz/fuzz_frames.cpp`, `fuzz/fuzz_messages.cpp`, `fuzz/fuzz_qr.cpp`, `fuzz/fuzz_handshake.cpp`, `fuzz/fuzz_media_meta.cpp` (Plan A Task 2 parsers), `fuzz/fuzz_sniff.cpp` (Plan A Task 1), `fuzz/corpus/<target>/` seeds; Modify `.github/workflows/ci.yml` (job `fuzz`: ubuntu-24.04, clang, `-fsanitize=fuzzer,address,undefined`, each target `-max_total_time=120` on push, corpus cached)

- [ ] **Step 1:** each target is `extern "C" int LLVMFuzzerTestOneInput(const uint8_t* d, size_t n)` calling the decoder and, when it succeeds, re-encoding and asserting `decode(encode(x)) == x` (messages, QR).
- [ ] **Step 2:** run each locally in WSL-less Windows? No: libFuzzer on MSVC (`/fsanitize=fuzzer`) works for frames/messages/qr; run 5 minutes each with the VS 2022 toolchain, record exec/s and crashes (expected 0) in `docs/spikes.md` style notes in `fuzz/README.md`.
- [ ] **Step 3: Commit** `test(fuzz): fuzz every parser that reads phone or file bytes`.

---

### Task 7: Android companion rewrite

**Files:**
- Rename `android-companion/` -> `android/` (git mv), keep the Gradle wrapper; delete `HttpCodec.kt`, `GalleryApi.kt`, `BluetoothGallerySession.kt`, `PairingPayload.kt` and their tests.
- Create (package `app.pms.companion`): `link/Frames.kt`, `link/Messages.kt`, `link/Base32.kt`, `link/PairingCode.kt`, `link/NoiseSession.kt` (noise-java IK initiator, prologue, payload kinds, SAS), `link/Transport.kt` (insecure RFCOMM to the QR MAC + service UUID), `media/Catalog.kt` (MediaStore paging by `GENERATION_MODIFIED` + `getVersion`, removed-id detection via a persisted id set, IS_FAVORITE), `media/Previews.kt` (`loadThumbnail` 256 px JPEG q80, <= 60 KB, re-encode lower if larger), `media/Keyframes.kt` (`MediaMetadataRetriever.getScaledFrameAtTime`, 8 frames), `media/Hashes.kt` (SHA-256 streaming via `ContentResolver.openInputStream`), `media/Trash.kt` (re-validate size/date/hash, refuse favorites, `createTrashRequest` chunks of 500 via an Activity result), `service/LinkService.kt` (foreground service type `connectedDevice`, notification with progress, partial wake lock with 10 min timeout renewed per request), `ui/ScanScreen.kt` (CameraX + zxing-cpp `BarcodeReader`), `ui/PairScreen.kt` (shows the 6 digits to compare), `ui/SessionScreen.kt` (progress, Disconnect), `ui/theme/` (tokens from `design/tokens.json`)
- Tests: `app/src/test/kotlin/app/pms/companion/` JVM tests for Frames, Messages (byte-for-byte against `core/tests/fixtures/golden_messages.bin` written by a Task 1 test helper), Base32/PairingCode (same vectors as Task 2), NoiseSession (replays `golden_ik_transcript.bin` from Task 3 and must produce identical msg1, accept msg2, and decrypt/encrypt the transport messages identically), Catalog diff logic (pure function over two snapshots), Trash planner (favorites refused, chunking 500)
- Manifest: `CAMERA`, `BLUETOOTH_CONNECT` (API 31+), `BLUETOOTH` (maxSdkVersion 30), `READ_MEDIA_IMAGES`, `READ_MEDIA_VIDEO`, `READ_MEDIA_VISUAL_USER_SELECTED`, `READ_EXTERNAL_STORAGE` (maxSdkVersion 32), `FOREGROUND_SERVICE`, `FOREGROUND_SERVICE_CONNECTED_DEVICE`, `POST_NOTIFICATIONS`, `WAKE_LOCK`. No INTERNET, no ACCESS_NETWORK_STATE, `android:allowBackup="false"`, `dataExtractionRules` excluding everything.
- Gradle: add `org.signal.forks:noise-java:0.1.1`, `io.github.zxing-cpp:android:<latest on Maven Central, pinned>`, CameraX `camera-camera2`, `camera-lifecycle`, `camera-view`; remove `zxing-android-embedded`; enable `dependencyVerification` (`gradle/verification-metadata.xml` with SHA-256 for every artifact).
- CI: `testReleaseUnitTest lintRelease assembleRelease` (unsigned release APK uploaded), plus a step that runs `aapt2 dump permissions` on the APK and fails if `android.permission.INTERNET` appears.

- [ ] **Step 1:** golden interop tests first (they fail: classes missing).
- [ ] **Step 2:** implement link package until golden tests pass byte-for-byte.
- [ ] **Step 3:** media package with JVM tests for the pure parts (diff, planner, preview size fallback ladder 256 px q80 -> q65 -> 224 px q65).
- [ ] **Step 4:** service + UI; lint clean; manual run on the user's phone happens in Plan B acceptance (S9).
- [ ] **Step 5: Commit** `feat(android): Noise-secured Bluetooth companion with CameraX scanning and system Trash`.

---

### Task 8: Acceptance on a real phone (with the user)

**Files:** Create `docs/acceptance/android-link.md` (checklist with results), `tools/push_test_album.py` (adb push the "PMS Test" album and mark 20 favorites through a debug-only instrumented helper `android/app/src/androidTest/.../MarkFavorites.kt` using `MediaStore.createFavoriteRequest`)

- [ ] Pair by QR, compare the 6 digits, sync 1,000+ items, measure KB/s both ways and record it, reconnect after desktop restart without QR, rotate the phone mid-transfer, toggle Wi-Fi during transfer, trash 30 items from the test album and confirm they appear in Android's Trash, restore one from Android's Trash, attempt a second pairing with the same QR from another phone (must be refused).
- [ ] Commit the filled checklist `test(acceptance): Android QR + Bluetooth link on real hardware`.

## Self-review notes

- Spec coverage: QR content and lifetime (Task 2), Noise IK with token/pinning/SAS (3), frames/messages/caps (1), windowed client + catalog resume + Missing handling (4), single-session RFCOMM server with DoS guards (5), fuzzing (6), Android rewrite incl. no INTERNET, FGS, CameraX/zxing-cpp, trash binding and chunking (7), hardware acceptance (8).
- Dependencies on Plan A: Task 6 fuzzes Plan A Tasks 1-2 parsers. Desktop UI for pairing/SAS/unpair lives in Plan C.
