#pragma once

#include <cstdint>

namespace pms {

// 8-bit luma, row-major; row y starts at pixels + y * stride, and bytes past `width` in a row are never read.
struct GrayView { const std::uint8_t* pixels{}; std::uint32_t width{}, height{}, stride{}; };

struct Quality {
    float sharpness{};        // max over a 4x4 tile grid of variance of the 3x3 Laplacian (bokeh-safe)
    float clipped_fraction{}; // share of pixels <= 2 or >= 253
    float luma_p1{}, luma_p99{};
    float noise_sigma{};      // Immerkaer (1996) estimator
};

// The caller downsamples to <= 512 px on the long side; cost is linear in pixels and nothing is allocated.
// Returns zeros for images smaller than 8x8, a null `pixels` or a `stride` shorter than `width`.
[[nodiscard]] Quality measure_quality(GrayView image) noexcept;

}  // namespace pms
