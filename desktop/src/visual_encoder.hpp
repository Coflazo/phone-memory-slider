#pragma once

#include <QImage>

#include <cstdint>

#include "pms/media_analysis.hpp"

namespace pms::desktop {

struct VisualEncoding {
    FixedEmbedding embedding;
    std::uint64_t perceptualHash{};
    float blurProblem{};
    float exposureProblem{};
    float screenshotLikelihood{};
    bool facePresent{};
};

class VisualEncoder final {
public:
    [[nodiscard]] static VisualEncoding encode(
        const QImage& image,
        bool video,
        std::uint64_t bytes,
        std::uint64_t duration_ms);
};

}  // namespace pms::desktop
