#include "pms/ranking.hpp"

#include <algorithm>
#include <cmath>

namespace pms {
namespace {

[[nodiscard]] float normalized(const float value) noexcept {
    return std::isfinite(value) ? std::clamp(value, 0.0F, 1.0F) : 0.0F;
}

}  // namespace

std::vector<RankedAsset> rank_assets(
    const std::span<const AssetFeatures> assets,
    const RankingPolicy policy) {
    std::vector<RankedAsset> ranked;
    ranked.reserve(assets.size());

    const auto cleanup_weight = normalized(policy.cleanup_weight);
    const auto mismatch_weight = normalized(policy.personal_mismatch_weight);
    const auto storage_weight = normalized(policy.storage_weight);
    const auto weight_total = cleanup_weight + mismatch_weight + storage_weight;

    for (const auto& asset : assets) {
        const auto protected_asset = asset.favorite || asset.explicit_keep;
        const auto keep_score = normalized(asset.keep_score);
        const auto weighted = cleanup_weight * normalized(asset.cleanup_confidence) +
                              mismatch_weight * (1.0F - keep_score) +
                              storage_weight * normalized(asset.storage_benefit);
        const auto priority = protected_asset || weight_total == 0.0F
                                  ? 0.0F
                                  : weighted / weight_total;
        ranked.push_back(RankedAsset{asset.asset_id, priority, protected_asset});
    }

    std::ranges::sort(ranked, [](const RankedAsset& left, const RankedAsset& right) {
        if (left.protected_asset != right.protected_asset) {
            return !left.protected_asset;
        }
        if (left.priority != right.priority) {
            return left.priority > right.priority;
        }
        return left.asset_id < right.asset_id;
    });
    return ranked;
}

}  // namespace pms

