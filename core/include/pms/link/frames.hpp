#pragma once

#include <array>
#include <cstddef>
#include <deque>
#include <optional>
#include <span>
#include <vector>

namespace pms::link {

// Wire frame: u16 big-endian payload length, then the payload. See docs/PROTOCOL.md.
inline constexpr std::size_t max_frame = 65535;
inline constexpr std::size_t max_preauth_frame = 512;

// Throws std::length_error if the payload is above max_frame.
[[nodiscard]] std::vector<std::byte> encode_frame(std::span<const std::byte> payload);

// Splits a byte stream into frames. Each length is checked against the cap as soon as its two
// header bytes arrive, before anything is allocated for it, so the frame being assembled never
// holds more than cap + 2 bytes. Frames completed by one feed() wait, in order, for next().
class FrameDecoder {
public:
    explicit FrameDecoder(std::size_t max_payload) noexcept;
    void set_max_payload(std::size_t max_payload) noexcept;
    // Appends bytes; returns false (and stays failed) on a length above the cap.
    [[nodiscard]] bool feed(std::span<const std::byte> bytes);
    // The oldest complete frame, or nullopt if none is complete or the decoder has failed.
    [[nodiscard]] std::optional<std::vector<std::byte>> next();
    [[nodiscard]] bool failed() const noexcept;

private:
    std::size_t max_payload_;
    std::array<std::byte, 2> header_{};
    std::size_t header_size_{0};
    std::size_t expected_{0};
    std::vector<std::byte> partial_;
    std::deque<std::vector<std::byte>> ready_;
    bool failed_{false};
};

}  // namespace pms::link
