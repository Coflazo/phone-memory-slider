#include "pms/link/qr.hpp"

#include <algorithm>
#include <cstddef>

namespace pms::link {
namespace {

constexpr std::string_view alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
constexpr std::string_view pairing_prefix = "PMS1:";
constexpr std::uint8_t pairing_version = 1;
constexpr std::size_t pairing_payload = 75;
constexpr std::size_t pairing_chars = 120;  // 75 bytes = 600 bits = exactly 120 five-bit characters

// The 5-bit value of an alphabet character, or -1 for anything else.
[[nodiscard]] int base32_value(const char ch) noexcept {
    if (ch >= 'A' && ch <= 'Z') {
        return ch - 'A';
    }
    if (ch >= '2' && ch <= '7') {
        return ch - '2' + 26;
    }
    return -1;
}

}  // namespace

std::string base32_encode(const std::span<const std::uint8_t> bytes) {
    std::string out;
    out.reserve((bytes.size() * 8 + 4) / 5);
    std::uint32_t buffer = 0;  // holds fewer than 5 pending bits between bytes
    std::size_t bits = 0;
    for (const auto byte : bytes) {
        buffer = (buffer << 8U) | byte;
        bits += 8;
        while (bits >= 5) {
            bits -= 5;
            out.push_back(alphabet[(buffer >> bits) & 0x1FU]);
        }
        buffer &= (1U << bits) - 1U;
    }
    if (bits > 0) {
        out.push_back(alphabet[(buffer << (5 - bits)) & 0x1FU]);  // zero-filled leftover bits
    }
    return out;
}

std::optional<std::vector<std::uint8_t>> base32_decode(const std::string_view text) noexcept {
    try {
        std::vector<std::uint8_t> out;
        out.reserve(text.size() / 8 * 5 + 4);  // bounded by the input
        std::uint32_t buffer = 0;
        std::size_t bits = 0;
        for (const char ch : text) {
            const int value = base32_value(ch);
            if (value < 0) {
                return std::nullopt;
            }
            buffer = (buffer << 5U) | static_cast<std::uint32_t>(value);
            bits += 5;
            if (bits >= 8) {
                bits -= 8;
                out.push_back(static_cast<std::uint8_t>(buffer >> bits));
                buffer &= (1U << bits) - 1U;
            }
        }
        // 5 or more leftover bits mean a character too many (length 1, 3 or 6 mod 8); non-zero ones are non-canonical.
        if (bits >= 5 || buffer != 0) {
            return std::nullopt;
        }
        return out;
    } catch (...) {
        return std::nullopt;  // only allocation can throw
    }
}

std::string encode_pairing(const PairingCode& code) {
    std::vector<std::uint8_t> payload{pairing_version};
    payload.reserve(pairing_payload);
    payload.insert(payload.end(), code.mac.begin(), code.mac.end());
    payload.insert(payload.end(), code.service_uuid.begin(), code.service_uuid.end());
    payload.insert(payload.end(), code.desktop_key.begin(), code.desktop_key.end());
    payload.insert(payload.end(), code.token.begin(), code.token.end());
    for (std::uint32_t shift = 32; shift > 0;) {
        shift -= 8;
        payload.push_back(static_cast<std::uint8_t>(code.expiry_unix_s >> shift));
    }
    return std::string{pairing_prefix} + base32_encode(payload);
}

std::optional<PairingCode> decode_pairing(const std::string_view text, const std::uint32_t now_unix_s) noexcept {
    if (text.size() != pairing_prefix.size() + pairing_chars || !text.starts_with(pairing_prefix)) {
        return std::nullopt;
    }
    const auto payload = base32_decode(text.substr(pairing_prefix.size()));
    if (!payload || payload->size() != pairing_payload || payload->front() != pairing_version) {
        return std::nullopt;
    }
    PairingCode code;
    std::span<const std::uint8_t> rest = std::span{*payload}.subspan(1);
    const auto take = [&rest](auto& field) noexcept {
        std::ranges::copy(rest.first(field.size()), field.begin());
        rest = rest.subspan(field.size());
    };
    take(code.mac);
    take(code.service_uuid);
    take(code.desktop_key);
    take(code.token);
    for (const auto byte : rest) {
        code.expiry_unix_s = (code.expiry_unix_s << 8U) | byte;
    }
    if (code.expiry_unix_s < now_unix_s) {
        return std::nullopt;
    }
    return code;
}

}  // namespace pms::link
