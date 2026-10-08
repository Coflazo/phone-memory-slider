#pragma once

// Internal to the link library (and its tests): the one place that calls noise-c.

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "pms/link/session.hpp"

struct NoiseHandshakeState_s;
struct NoiseCipherState_s;

namespace pms::link {
PMS_LINK_ABI_BEGIN
namespace detail {

enum class Role : std::uint8_t { Initiator, Responder };

inline constexpr std::size_t tag_size = 16;  // ChaChaPoly authentication tag
// The most a handshake message adds to its payload: message 1's ephemeral key, encrypted static key and tag.
inline constexpr std::size_t max_handshake_overhead = 32 + (32 + tag_size) + tag_size;

// One Noise_IK_25519_ChaChaPoly_SHA256 handshake, then its two transport ciphers. Fails closed: the first failed
// call frees every key and makes all later calls fail.
class NoiseIk {
public:
    // remote_static: the responder's public key, for the initiator; nullptr for the responder. Throws
    // std::invalid_argument if local's public key does not match its private key, std::runtime_error if noise-c fails.
    NoiseIk(Role role, const KeyPair& local, std::span<const std::uint8_t> prologue, const Key32* remote_static);
    ~NoiseIk();
    NoiseIk(const NoiseIk&) = delete;
    NoiseIk& operator=(const NoiseIk&) = delete;

    // Handshake messages in turn; nullopt on the wrong turn, a bad tag or a malformed message. After the last one the
    // handshake splits into the transport ciphers and established() turns true.
    [[nodiscard]] std::optional<std::vector<std::byte>> write_message(std::span<const std::byte> payload);
    [[nodiscard]] std::optional<std::vector<std::byte>> read_message(std::span<const std::byte> message);
    [[nodiscard]] bool established() const noexcept;
    [[nodiscard]] std::optional<Key32> remote_static() const;  // once known: from the start (initiator), after msg1 (responder)
    [[nodiscard]] const std::array<std::uint8_t, 32>& handshake_hash() const noexcept;  // meaningful once established

    // Transport, once established; nullopt on any failure (oversize, bad tag), which also closes.
    [[nodiscard]] std::optional<std::vector<std::byte>> seal(std::span<const std::byte> plaintext);
    [[nodiscard]] std::optional<std::vector<std::byte>> open(std::span<const std::byte> ciphertext);
    void close() noexcept;

private:
    struct HandshakeFree { void operator()(NoiseHandshakeState_s* state) const noexcept; };
    struct CipherFree { void operator()(NoiseCipherState_s* state) const noexcept; };

    [[nodiscard]] bool after_handshake_step();

    std::unique_ptr<NoiseHandshakeState_s, HandshakeFree> handshake_;
    std::unique_ptr<NoiseCipherState_s, CipherFree> send_;
    std::unique_ptr<NoiseCipherState_s, CipherFree> receive_;
    std::array<std::uint8_t, 32> handshake_hash_{};
    std::optional<Key32> remote_static_;
};

[[nodiscard]] Key32 x25519_public_key(const Key32& private_key);
// True for the all-zero key and for every key whose X25519 output with a clamped scalar is all zeros (the points of
// small order, in any encoding): a peer claiming one needs no private key, since every DH result with it is zero.
[[nodiscard]] bool small_order_public_key(const Key32& public_key);
// Zeroes memory with volatile writes the compiler cannot drop (noise-c's noise_clean).
void secure_zero(void* data, std::size_t size) noexcept;
// docs/PROTOCOL.md section 4: u32 BE of the first 4 hash bytes, modulo 1,000,000, as 6 zero-padded digits.
[[nodiscard]] std::string sas_digits(std::span<const std::uint8_t, 32> handshake_hash);
[[nodiscard]] bool equal_constant_time(std::span<const std::uint8_t> a, std::span<const std::uint8_t> b) noexcept;

}  // namespace detail
PMS_LINK_ABI_END
}  // namespace pms::link
