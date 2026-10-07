#include "pms/pdq.hpp"

#include <bit>
#include <climits>
#include <cstddef>
#include <memory>

#include <pdq/downscaling/downscaling.h>
#include <pdq/hashing/pdqhashing.h>

namespace pms {
namespace {

constexpr std::uint32_t min_side = 5;  // MIN_HASHABLE_DIM in pdqhashing.cpp; PDQ itself returns zeros below it

}  // namespace

PdqHash pdq_hash(const RgbView image) {
    const std::uint32_t width = image.width, height = image.height;
    // PDQ indexes pixels and rows with int, so the pixel count and the row stride must fit one.
    if (image.pixels == nullptr || width < min_side || height < min_side || image.stride > INT_MAX ||
        std::uint64_t{image.stride} < std::uint64_t{width} * 3 || std::uint64_t{width} * height > INT_MAX)
        return {};
    const int rows = static_cast<int>(height), cols = static_cast<int>(width);
    const std::size_t pixels = std::size_t{width} * height;

    // pdqHash256FromFloatLuma blurs in place: buffer 1 holds the luma and is overwritten, buffer 2 is scratch
    // that its row blur fully writes before reading, so neither needs zeroing.
    const auto luma = std::make_unique_for_overwrite<float[]>(2 * pixels);
    facebook::pdq::downscaling::fillFloatLumaFromRGB(image.pixels, image.pixels + 1, image.pixels + 2, rows, cols,
                                                     static_cast<int>(image.stride), 3, luma.get());
    float buffer64x64[64][64];
    float buffer16x64[16][64];
    float buffer16x16[16][16];
    facebook::pdq::hashing::Hash256 hash;
    PdqHash result;
    facebook::pdq::hashing::pdqHash256FromFloatLuma(luma.get(), luma.get() + pixels, rows, cols, buffer64x64,
                                                    buffer16x64, buffer16x16, hash, result.quality);
    // 16-bit word w[i] carries hash bits 16i..16i+15, so four consecutive words make one 64-bit word.
    for (std::size_t i = 0; i < 16; ++i)
        result.bits[i / 4] |= std::uint64_t{hash.w[i]} << (16 * (i % 4));
    return result;
}

int hamming(const PdqHash& a, const PdqHash& b) noexcept {
    int distance = 0;
    for (std::size_t i = 0; i < a.bits.size(); ++i) distance += std::popcount(a.bits[i] ^ b.bits[i]);
    return distance;
}

}  // namespace pms
