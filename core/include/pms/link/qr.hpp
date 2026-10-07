#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace pms::link {

// Pairing QR text: "PMS1:" + Base32 of u8 version (1) | MAC | service UUID | desktop static X25519 key | one-time
// token | u32 BE expiry, 75 bytes as 120 characters. Byte layout in docs/PROTOCOL.md section 3.
struct PairingCode {
    std::array<std::uint8_t, 6> mac{};
    std::array<std::uint8_t, 16> service_uuid{};
    std::array<std::uint8_t, 32> desktop_key{};
    std::array<std::uint8_t, 16> token{};
    std::uint32_t expiry_unix_s{};

    bool operator==(const PairingCode&) const = default;
};

// RFC 4648 Base32: alphabet A-Z 2-7, uppercase, no padding.
[[nodiscard]] std::string base32_encode(std::span<const std::uint8_t> bytes);
// Accepts only canonical text: alphabet characters, a length base32_encode can produce, and zero leftover bits.
// So anything it accepts re-encodes to exactly the same text.
[[nodiscard]] std::optional<std::vector<std::uint8_t>> base32_decode(std::string_view text) noexcept;

[[nodiscard]] std::string encode_pairing(const PairingCode& code);  // "PMS1:" + 120 chars
// nullopt for anything but exactly "PMS1:" + 120 canonical Base32 characters of a version-1 payload, or when
// expiry_unix_s < now_unix_s. The phone passes now_unix_s = 0 (no expiry check): the desktop checks the window
// against its own clock (PairingWindow), and single use is the session's job too (ResponderSession burns the window).
[[nodiscard]] std::optional<PairingCode> decode_pairing(std::string_view text, std::uint32_t now_unix_s) noexcept;

}  // namespace pms::link
