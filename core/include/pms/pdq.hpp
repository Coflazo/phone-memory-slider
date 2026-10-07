#pragma once

#include <array>
#include <cstdint>

namespace pms {

// PDQ 256-bit perceptual hash (third_party/pdq). Bit k of PDQ's hash (k = 16 * row + column of its 16x16
// DCT block) is bit (k % 64) of bits[k / 64]. PDQ's 64-digit hex form is bits[3], bits[2], bits[1],
// bits[0], each printed as 16 hex digits. quality is PDQ's 0..100 gradient score; below 50 PDQ treats
// the hash as unreliable.
struct PdqHash { std::array<std::uint64_t, 4> bits{}; int quality{}; };

// Interleaved RGB8; row y starts at pixels + y * stride, and bytes past width * 3 in a row are never read.
struct RgbView { const std::uint8_t* pixels{}; std::uint32_t width{}, height{}, stride{}; };

// The caller passes <= 512 px on the long side (PDQ's own reference resizes to that before hashing).
// Prefer a width that is not a power of two: PDQ is about 3x slower at exactly 512 px wide than at 500-511 px,
// because its column blur hits cache set conflicts at a power-of-two row pitch.
// Thread-safe: scratch is allocated per call (2 * width * height floats plus about 21 KB of stack).
// Returns an empty hash with quality 0 for a null `pixels`, a stride shorter than width * 3 or above INT_MAX,
// or a side below PDQ's 5 px minimum or above 4096 px.
[[nodiscard]] PdqHash pdq_hash(RgbView image);

[[nodiscard]] int hamming(const PdqHash& a, const PdqHash& b) noexcept;

}  // namespace pms
