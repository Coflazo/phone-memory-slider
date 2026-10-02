#include "pms/media_analysis.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <unordered_map>

namespace pms {
namespace {

[[nodiscard]] float normalized(const float value) noexcept {
    return std::isfinite(value) ? std::clamp(value, 0.0F, 1.0F) : 0.0F;
}

[[nodiscard]] bool preferred_representative(
    const MediaObservation& candidate,
    const float candidate_score,
    const MediaObservation& current,
    const float current_score) noexcept {
    if (candidate.favorite != current.favorite) {
        return candidate.favorite;
    }
    if (candidate.explicit_keep != current.explicit_keep) {
        return candidate.explicit_keep;
    }
    if (candidate_score != current_score) {
        return candidate_score > current_score;
    }
    return candidate.asset_id < current.asset_id;
}

}  // namespace

FixedEmbedding::FixedEmbedding(const std::initializer_list<float> values) noexcept
    : FixedEmbedding(std::span<const float>{values.begin(), values.size()}) {}

FixedEmbedding::FixedEmbedding(const std::span<const float> values) noexcept {
    const auto count = std::min(values.size(), values_.size());
    for (std::size_t index = 0; index < count; ++index) {
        values_[index] = normalized(values[index]);
    }
}

float FixedEmbedding::operator[](const std::size_t index) const noexcept {
    return values_[index];
}

std::span<const float, embedding_dimensions> FixedEmbedding::values() const noexcept {
    return values_;
}

bool AnalysisResult::has_reason(const ReasonCode reason) const noexcept {
    return std::ranges::find(reasons, reason) != reasons.end();
}

std::vector<AnalysisResult> analyze_media(
    const std::span<const MediaObservation> observations,
    const std::span<const float> personal_scores) {
    const auto has_personal_scores = personal_scores.size() == observations.size();
    std::unordered_map<std::uint64_t, std::size_t> perceptual_representatives;
    std::unordered_map<std::string, std::size_t> exact_representatives;
    perceptual_representatives.reserve(observations.size());
    exact_representatives.reserve(observations.size());

    for (std::size_t index = 0; index < observations.size(); ++index) {
        const auto& observation = observations[index];
        const auto personal_score = has_personal_scores ? normalized(personal_scores[index]) : 0.5F;
        if (observation.perceptual_hash != 0) {
            const auto [position, inserted] = perceptual_representatives.try_emplace(observation.perceptual_hash, index);
            if (!inserted) {
                const auto current_score = has_personal_scores ? normalized(personal_scores[position->second]) : 0.5F;
                if (preferred_representative(
                        observation, personal_score, observations[position->second], current_score)) {
                    position->second = index;
                }
            }
        }
        if (observation.perceptual_hash != 0 && !observation.content_digest.empty()) {
            auto key = std::to_string(observation.perceptual_hash) + ':' + observation.content_digest;
            const auto [position, inserted] = exact_representatives.try_emplace(std::move(key), index);
            if (!inserted) {
                const auto current_score = has_personal_scores ? normalized(personal_scores[position->second]) : 0.5F;
                if (preferred_representative(
                        observation, personal_score, observations[position->second], current_score)) {
                    position->second = index;
                }
            }
        }
    }

    std::vector<AnalysisResult> results;
    results.reserve(observations.size());
    for (std::size_t index = 0; index < observations.size(); ++index) {
        const auto& observation = observations[index];
        const auto personal_score = has_personal_scores ? normalized(personal_scores[index]) : 0.5F;
        AnalysisResult result{
            .features = {
                observation.asset_id,
                0.0F,
                personal_score,
                normalized(observation.storage_benefit),
                observation.favorite,
                observation.explicit_keep,
            },
        };

        if (observation.favorite || observation.explicit_keep) {
            results.push_back(std::move(result));
            continue;
        }

        auto cleanup_confidence = 0.0F;
        auto exact_duplicate = false;
        if (observation.perceptual_hash != 0 && !observation.content_digest.empty()) {
            const auto key = std::to_string(observation.perceptual_hash) + ':' + observation.content_digest;
            if (const auto found = exact_representatives.find(key);
                found != exact_representatives.end() && found->second != index) {
                result.reasons.push_back(ReasonCode::ExactDuplicate);
                cleanup_confidence = 1.0F;
                exact_duplicate = true;
            }
        }
        if (!exact_duplicate && observation.perceptual_hash != 0) {
            if (const auto found = perceptual_representatives.find(observation.perceptual_hash);
                found != perceptual_representatives.end() && found->second != index) {
                result.reasons.push_back(ReasonCode::NearDuplicate);
                cleanup_confidence = std::max(cleanup_confidence, 0.78F);
            }
        }
        if (normalized(observation.blur) >= 0.75F) {
            result.reasons.push_back(ReasonCode::Blurry);
            cleanup_confidence = std::max(cleanup_confidence, normalized(observation.blur));
        }
        if (normalized(observation.screenshot_likelihood) >= 0.80F) {
            result.reasons.push_back(ReasonCode::ScreenCapture);
            cleanup_confidence = std::max(cleanup_confidence, normalized(observation.screenshot_likelihood));
        }
        if (observation.video && normalized(observation.storage_benefit) >= 0.75F) {
            result.reasons.push_back(ReasonCode::LargeVideo);
            cleanup_confidence = std::max(cleanup_confidence, normalized(observation.storage_benefit));
        }
        if (has_personal_scores && personal_score <= 0.25F) {
            result.reasons.push_back(ReasonCode::LowPersonalMatch);
        }
        result.features.cleanup_confidence = cleanup_confidence;
        results.push_back(std::move(result));
    }
    return results;
}

}  // namespace pms
