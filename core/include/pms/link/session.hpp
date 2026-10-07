#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace pms::link {

// Noise_IK_25519_ChaChaPoly_SHA256 between the phone (initiator) and the desktop (responder), with the prologue
// "PMS/1" + service UUID. Message 1's payload, the pairing token, pinning and the 6-digit code are in
// docs/PROTOCOL.md section 4. Both sessions fail closed: after any failure every later call fails too.

using Key32 = std::array<std::uint8_t, 32>;
struct KeyPair { Key32 public_key{}; Key32 private_key{}; };
[[nodiscard]] KeyPair generate_keypair();  // X25519 from the OS CSPRNG

struct Pinned { Key32 phone_key{}; std::string model; std::int64_t paired_unix_s{}; };
class PinStore {
public:
    virtual ~PinStore() = default;
    [[nodiscard]] virtual std::optional<Pinned> find(const Key32& phone_key) const = 0;
    virtual void pin(const Pinned& p) = 0;
    virtual void unpin(const Key32& phone_key) = 0;
};

enum class HandshakeResult : std::uint8_t { Continue, Established, Rejected };

namespace detail { class NoiseIk; }

// Desktop side, one per connection.
class ResponderSession {
public:
    ResponderSession(KeyPair desktop, std::array<std::uint8_t, 16> service_uuid, PinStore& pins);
    ~ResponderSession();
    ResponderSession(const ResponderSession&) = delete;
    ResponderSession& operator=(const ResponderSession&) = delete;

    // Accept a first pairing (msg1 kind 1) with this token through expiry_unix_s inclusive, by this session's clock.
    void open_pairing_window(const std::array<std::uint8_t, 16>& token, std::uint32_t expiry_unix_s);
    // msg1 plaintext payload = u8 kind (1 = first pairing with token, 2 = reconnect) | 16-byte token (kind 1) | string model.
    // Established: msg2_out holds message 2. Kind 1 pinned the phone's key and burned the token (call
    // open_pairing_window again for another pairing); kind 2 needed an existing pin. Rejected: msg2_out is empty,
    // nothing was pinned or burned, and the session is dead. One call per session; it never returns Continue.
    // An exception from PinStore propagates and leaves the session dead.
    [[nodiscard]] HandshakeResult on_message1(std::span<const std::byte> msg1, std::uint32_t now_unix_s,
                                              std::vector<std::byte>& msg2_out);
    [[nodiscard]] bool first_pairing() const noexcept;  // Established by kind 1: the token is spent; show sas()
    // After Established (std::logic_error before).
    [[nodiscard]] std::string sas() const;  // 6 digits from the handshake hash
    [[nodiscard]] const Pinned& peer() const;
    // Transport. seal throws std::length_error above max_plaintext and std::logic_error unless established and open.
    [[nodiscard]] std::vector<std::byte> seal(std::span<const std::byte> plaintext);
    [[nodiscard]] std::optional<std::vector<std::byte>> open(std::span<const std::byte> ct);  // nullopt = close session

private:
    struct Window { std::array<std::uint8_t, 16> token{}; std::uint32_t expiry_unix_s{}; };

    KeyPair desktop_;
    std::array<std::uint8_t, 16> service_uuid_;
    PinStore& pins_;
    std::optional<Window> window_;
    std::unique_ptr<detail::NoiseIk> noise_;
    Pinned peer_;
    bool used_{false};
    bool established_{false};
    bool first_pairing_{false};
};

// Phone side. The Android app mirrors it in Kotlin; here it serves tests and the golden transcript.
class InitiatorSession {
public:
    InitiatorSession(KeyPair phone, Key32 desktop_static, std::array<std::uint8_t, 16> service_uuid);
    ~InitiatorSession();
    InitiatorSession(const InitiatorSession&) = delete;
    InitiatorSession& operator=(const InitiatorSession&) = delete;

    // One message 1 per session (std::logic_error on a second). Throw std::invalid_argument for a model over 1024
    // bytes or not UTF-8, and std::length_error if message 1 would not fit one 512-byte pre-handshake frame.
    [[nodiscard]] std::vector<std::byte> message1_first_pairing(const std::array<std::uint8_t, 16>& token,
                                                                std::string_view model);
    [[nodiscard]] std::vector<std::byte> message1_reconnect(std::string_view model);
    [[nodiscard]] bool on_message2(std::span<const std::byte> msg2);  // false: wrong desktop or tampered; dead
    [[nodiscard]] std::string sas() const;                           // after on_message2 returned true
    [[nodiscard]] std::vector<std::byte> seal(std::span<const std::byte> plaintext);
    [[nodiscard]] std::optional<std::vector<std::byte>> open(std::span<const std::byte> ct);  // nullopt = close session

private:
    [[nodiscard]] std::vector<std::byte> message1(std::vector<std::byte> payload);

    std::unique_ptr<detail::NoiseIk> noise_;
    bool sent_{false};
    bool established_{false};
};

#ifdef PMS_LINK_TEST_HOOKS
// Test hook: handshakes started while set use these ephemeral keys instead of fresh random ones (golden transcripts,
// cacophony vectors). Exists only in pms_link_testing, the copy of this library built for pms_core_tests; the app
// links pms_link, which is compiled without PMS_LINK_TEST_HOOKS and has no such function or code path.
void set_fixed_ephemeral_for_tests(std::optional<KeyPair> responder, std::optional<KeyPair> initiator);
#endif

}  // namespace pms::link
