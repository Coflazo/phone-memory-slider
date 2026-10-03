#include "visual_encoder.hpp"

#include <QColor>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

#include "face_presence_detector.hpp"

namespace pms::desktop {
namespace {

[[nodiscard]] float clamp01(const double value) noexcept {
    return static_cast<float>(std::clamp(value, 0.0, 1.0));
}

[[nodiscard]] double luma(const QRgb pixel) noexcept {
    return (0.2126 * qRed(pixel) + 0.7152 * qGreen(pixel) + 0.0722 * qBlue(pixel)) / 255.0;
}

[[nodiscard]] std::uint64_t difference_hash(const QImage& image) {
    const auto sample = image.scaled(9, 8, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
                            .convertToFormat(QImage::Format_RGB32);
    std::uint64_t hash{};
    for (auto y = 0; y < 8; ++y) {
        const auto* row = reinterpret_cast<const QRgb*>(sample.constScanLine(y));
        for (auto x = 0; x < 8; ++x) {
            hash <<= 1U;
            hash |= luma(row[x]) < luma(row[x + 1]) ? 1U : 0U;
        }
    }
    return hash;
}

}  // namespace

VisualEncoding VisualEncoder::encode(
    const QImage& image,
    const bool video,
    const std::uint64_t bytes,
    const std::uint64_t duration_ms) {
    if (image.isNull()) {
        return {};
    }
    const auto sample = image.scaled(64, 64, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
                            .convertToFormat(QImage::Format_RGB32);
    constexpr auto count = 64.0 * 64.0;
    std::array<double, 16> histogram{};
    auto red_sum = 0.0;
    auto green_sum = 0.0;
    auto blue_sum = 0.0;
    auto luma_sum = 0.0;
    auto luma_squared_sum = 0.0;
    auto saturation_sum = 0.0;
    auto edge_sum = 0.0;
    auto dark_count = 0.0;
    auto bright_count = 0.0;
    for (auto y = 0; y < sample.height(); ++y) {
        const auto* row = reinterpret_cast<const QRgb*>(sample.constScanLine(y));
        const auto* previous = y > 0 ? reinterpret_cast<const QRgb*>(sample.constScanLine(y - 1)) : nullptr;
        for (auto x = 0; x < sample.width(); ++x) {
            const auto pixel = row[x];
            const auto red = qRed(pixel) / 255.0;
            const auto green = qGreen(pixel) / 255.0;
            const auto blue = qBlue(pixel) / 255.0;
            const auto luminosity = luma(pixel);
            red_sum += red;
            green_sum += green;
            blue_sum += blue;
            luma_sum += luminosity;
            luma_squared_sum += luminosity * luminosity;
            const auto maximum = std::max({red, green, blue});
            const auto minimum = std::min({red, green, blue});
            saturation_sum += maximum == 0.0 ? 0.0 : (maximum - minimum) / maximum;
            dark_count += luminosity < 0.05 ? 1.0 : 0.0;
            bright_count += luminosity > 0.95 ? 1.0 : 0.0;
            const auto bin = std::min(15, static_cast<int>(luminosity * 16.0));
            histogram[static_cast<std::size_t>(bin)] += 1.0;
            if (x > 0) {
                edge_sum += std::abs(luminosity - luma(row[x - 1]));
            }
            if (previous != nullptr) {
                edge_sum += std::abs(luminosity - luma(previous[x]));
            }
        }
    }
    const auto mean_luma = luma_sum / count;
    const auto luma_variance = std::max(0.0, luma_squared_sum / count - mean_luma * mean_luma);
    const auto edge_density = edge_sum / (count * 2.0);
    auto entropy = 0.0;
    for (const auto bin_count : histogram) {
        if (bin_count == 0.0) {
            continue;
        }
        const auto probability = bin_count / count;
        entropy -= probability * std::log2(probability);
    }
    const auto aspect = static_cast<double>(image.width()) / static_cast<double>(std::max(1, image.height()));
    const auto portrait = aspect < 0.8 ? 1.0 : 0.0;
    const auto mean_saturation = saturation_sum / count;
    const auto face_present = FacePresenceDetector::detect(image);
    const auto bytes_scale = std::log1p(static_cast<double>(bytes)) / std::log1p(8.0 * 1024 * 1024 * 1024);
    const auto duration_scale = static_cast<double>(duration_ms) / (10.0 * 60 * 1'000);
    const auto screenshot = !video && portrait > 0.0 && edge_density > 0.08 && mean_saturation < 0.30 ? 0.9 : 0.0;
    const std::array<float, embedding_dimensions> values{
        clamp01(red_sum / count),
        clamp01(green_sum / count),
        clamp01(blue_sum / count),
        clamp01(mean_luma),
        clamp01(std::sqrt(luma_variance) * 2.0),
        clamp01(mean_saturation),
        clamp01(edge_density * 4.0),
        clamp01(dark_count / count),
        clamp01(bright_count / count),
        clamp01(std::min(aspect, 1.0 / std::max(aspect, 0.001))),
        static_cast<float>(portrait),
        video ? 1.0F : 0.0F,
        clamp01(bytes_scale),
        clamp01(duration_scale),
        face_present ? 1.0F : 0.0F,
        clamp01(entropy / 4.0),
    };
    return {
        FixedEmbedding{values},
        difference_hash(image),
        clamp01(1.0 - edge_density * 5.0),
        clamp01(std::max(dark_count, bright_count) / count),
        static_cast<float>(screenshot),
        face_present,
    };
}

}  // namespace pms::desktop
