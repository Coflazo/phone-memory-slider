#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <string>
#include <vector>

#include "pms/review_session.hpp"

namespace pms {

inline constexpr std::size_t embedding_dimensions = 16;

class FixedEmbedding final {
public:
    FixedEmbedding() = default;
    explicit FixedEmbedding(std::initializer_list<float> values) noexcept;
    explicit FixedEmbedding(std::span<const float> values) noexcept;

    [[nodiscard]] float operator[](std::size_t index) const noexcept;
    [[nodiscard]] std::span<const float, embedding_dimensions> values() const noexcept;

private:
    std::array<float, embedding_dimensions> values_{};
};

struct MediaObservation {
    std::string asset_id;
    FixedEmbedding embedding;
    std::uint64_t perceptual_hash{};
    std::string content_digest;
    float blur{};
    float exposure_problem{};
    float screenshot_likelihood{};
    float storage_benefit{};
    bool video{};
    bool favorite{};
    bool explicit_keep{};
};

struct AnalysisResult {
    AssetFeatures features;
    std::vector<ReasonCode> reasons{};

    [[nodiscard]] bool has_reason(ReasonCode reason) const noexcept;
};

[[nodiscard]] std::vector<AnalysisResult> analyze_media(
    std::span<const MediaObservation> observations,
    std::span<const float> personal_scores = {});

}  // namespace pms
