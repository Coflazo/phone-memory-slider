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
inline constexpr std::size_t min_frame = 16;  // every frame carries a Noise message: at least a 16-byte tag

// Throws std::length_error if the payload is below min_frame or above max_frame.
[[nodiscard]] std::vector<std::byte> encode_frame(std::span<const std::byte> payload);

// Splits a byte stream into frames of min_frame..cap bytes. Each length is checked as soon as its two
// header bytes arrive, before anything is allocated for it. Frames completed by one feed() wait, in
// order, for next(), so the decoder holds at most one feed's bytes plus cap + 2: callers feed reads
// of <= 64 KiB and drain next() after every feed. While the cap is at most max_preauth_frame exactly
// one frame is accepted: a second frame that starts before next() has taken the first fails.
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
    bool fail() noexcept;

    std::size_t max_payload_;
    std::array<std::byte, 2> header_{};
    std::size_t header_size_{0};
    std::size_t expected_{0};
    std::vector<std::byte> partial_;
    std::deque<std::vector<std::byte>> ready_;
    bool failed_{false};
};

}  // namespace pms::link
