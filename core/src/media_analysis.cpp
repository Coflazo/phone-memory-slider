#include "pms/media_analysis.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <numeric>
#include <limits>
#include <string>
#include <unordered_map>
#include <unordered_set>

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
    std::unordered_map<std::string, std::size_t> exact_representatives;
    exact_representatives.reserve(observations.size());

    for (std::size_t index = 0; index < observations.size(); ++index) {
        const auto& observation = observations[index];
        const auto personal_score = has_personal_scores ? normalized(personal_scores[index]) : 0.5F;
        if (!observation.content_digest.empty()) {
            const auto [position, inserted] = exact_representatives.try_emplace(observation.content_digest, index);
            if (!inserted) {
                const auto current_score = has_personal_scores ? normalized(personal_scores[position->second]) : 0.5F;
                if (preferred_representative(
                        observation, personal_score, observations[position->second], current_score)) {
                    position->second = index;
                }
            }
        }
    }

    std::vector<std::size_t> parent(observations.size());
    std::iota(parent.begin(), parent.end(), 0);
    const auto find_root = [&parent](std::size_t index) {
        while (parent[index] != index) {
            parent[index] = parent[parent[index]];
            index = parent[index];
        }
        return index;
    };
    const auto join = [&parent, &find_root](const std::size_t left, const std::size_t right) {
        const auto left_root = find_root(left);
        const auto right_root = find_root(right);
        if (left_root != right_root) {
            parent[right_root] = left_root;
        }
    };
    std::unordered_map<std::uint32_t, std::vector<std::size_t>> buckets;
    buckets.reserve(observations.size() * 2);
    for (std::size_t index = 0; index < observations.size(); ++index) {
        const auto hash = observations[index].perceptual_hash;
        if (hash == 0) {
            continue;
        }
        std::unordered_set<std::size_t> candidates;
        for (std::uint32_t segment = 0; segment < 4; ++segment) {
            const auto value = static_cast<std::uint32_t>((hash >> (segment * 16U)) & 0xffffU);
            const auto key = (segment << 16U) | value;
            if (const auto found = buckets.find(key); found != buckets.end()) {
                candidates.insert(found->second.begin(), found->second.end());
            }
        }
        for (const auto candidate : candidates) {
            if (std::popcount(hash ^ observations[candidate].perceptual_hash) <= 6) {
                join(index, candidate);
            }
        }
        for (std::uint32_t segment = 0; segment < 4; ++segment) {
            const auto value = static_cast<std::uint32_t>((hash >> (segment * 16U)) & 0xffffU);
            buckets[(segment << 16U) | value].push_back(index);
        }
    }
    std::vector<std::size_t> perceptual_representatives(observations.size(), observations.size());
    for (std::size_t index = 0; index < observations.size(); ++index) {
        if (observations[index].perceptual_hash == 0) {
            continue;
        }
        const auto root = find_root(index);
        auto& representative = perceptual_representatives[root];
        if (representative == observations.size()) {
            representative = index;
            continue;
        }
        const auto candidate_score = has_personal_scores ? normalized(personal_scores[index]) : 0.5F;
        const auto current_score = has_personal_scores ? normalized(personal_scores[representative]) : 0.5F;
        if (preferred_representative(
                observations[index], candidate_score, observations[representative], current_score)) {
            representative = index;
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
        if (!observation.content_digest.empty()) {
            if (const auto found = exact_representatives.find(observation.content_digest);
                found != exact_representatives.end() && found->second != index) {
                result.reasons.push_back(ReasonCode::ExactDuplicate);
                cleanup_confidence = 1.0F;
                exact_duplicate = true;
            }
        }
        if (!exact_duplicate && observation.perceptual_hash != 0) {
            const auto representative = perceptual_representatives[find_root(index)];
            if (representative != observations.size() && representative != index) {
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
