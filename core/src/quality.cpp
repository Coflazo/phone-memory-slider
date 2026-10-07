#include "pms/quality.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <numbers>

namespace pms {
namespace {

constexpr std::uint32_t min_side = 8;
constexpr std::size_t tiles = 4;  // per axis

using Histogram = std::array<std::uint64_t, 256>;

// First luma value whose cumulative count reaches `share` of all pixels.
[[nodiscard]] float percentile(const Histogram& histogram, const std::uint64_t total, const double share) noexcept {
    const double target = share * static_cast<double>(total);
    std::uint64_t seen = 0;
    for (std::size_t value = 0; value < histogram.size(); ++value) {
        seen += histogram[value];
        if (static_cast<double>(seen) >= target) return static_cast<float>(value);
    }
    return 255.0F;
}

// Tile boundary i of `tiles` along an axis of `size` pixels, clamped to the interior [1, size - 1)
// where the 3x3 kernels fit. For size >= 8 every tile keeps at least one pixel.
[[nodiscard]] std::size_t tile_edge(const std::uint32_t size, const std::size_t i) noexcept {
    return std::clamp<std::size_t>(std::size_t{size} * i / tiles, 1, size - 1);
}

}  // namespace

Quality measure_quality(const GrayView image) noexcept {
    const std::uint32_t width = image.width, height = image.height;
    if (image.pixels == nullptr || width < min_side || height < min_side || image.stride < width) return {};
    const auto row = [&image](const std::size_t y) { return image.pixels + y * image.stride; };

    Quality quality;
    Histogram histogram{};
    for (std::size_t y = 0; y < height; ++y) {
        const std::uint8_t* const line = row(y);
        for (std::size_t x = 0; x < width; ++x) ++histogram[line[x]];
    }
    const std::uint64_t total = std::uint64_t{width} * height;
    const std::uint64_t clipped = histogram[0] + histogram[1] + histogram[2] + histogram[253] + histogram[254] + histogram[255];
    quality.clipped_fraction = static_cast<float>(static_cast<double>(clipped) / static_cast<double>(total));
    quality.luma_p1 = percentile(histogram, total, 0.01);
    quality.luma_p99 = percentile(histogram, total, 0.99);

    // One pass over the interior: Laplacian [0,1,0;1,-4,1;0,1,0] variance per tile, and the Immerkaer
    // sum of |I * N| with N = [1,-2,1;-2,4,-2;1,-2,1]. The tiles partition the interior exactly.
    double noise_sum = 0.0;
    double sharpest = 0.0;
    for (std::size_t ty = 0; ty < tiles; ++ty) {
        const std::size_t y0 = tile_edge(height, ty), y1 = tile_edge(height, ty + 1);
        for (std::size_t tx = 0; tx < tiles; ++tx) {
            const std::size_t x0 = tile_edge(width, tx), x1 = tile_edge(width, tx + 1);
            double sum = 0.0, sum_sq = 0.0;
            for (std::size_t y = y0; y < y1; ++y) {
                const std::uint8_t* const up = row(y - 1);
                const std::uint8_t* const mid = row(y);
                const std::uint8_t* const down = row(y + 1);
                for (std::size_t x = x0; x < x1; ++x) {
                    const int cross = up[x] + down[x] + mid[x - 1] + mid[x + 1];
                    const int corners = up[x - 1] + up[x + 1] + down[x - 1] + down[x + 1];
                    const int laplacian = cross - 4 * mid[x];
                    sum += laplacian;
                    sum_sq += static_cast<double>(laplacian) * laplacian;
                    noise_sum += std::abs(corners - 2 * cross + 4 * mid[x]);
                }
            }
            const double count = static_cast<double>((x1 - x0) * (y1 - y0));
            const double mean = sum / count;
            sharpest = std::max(sharpest, sum_sq / count - mean * mean);
        }
    }
    quality.sharpness = static_cast<float>(sharpest);
    const double interior = static_cast<double>(width - 2) * static_cast<double>(height - 2);
    quality.noise_sigma = static_cast<float>(std::sqrt(std::numbers::pi / 2.0) / (6.0 * interior) * noise_sum);
    return quality;
}

}  // namespace pms
