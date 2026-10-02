#pragma once

#include <span>
#include <string>
#include <vector>

namespace pms {

struct AssetFeatures {
    std::string asset_id;
    float cleanup_confidence{};
    float keep_score{};
    float storage_benefit{};
    bool favorite{};
    bool explicit_keep{};
};

struct RankingPolicy {
    float cleanup_weight{0.50F};
    float personal_mismatch_weight{0.35F};
    float storage_weight{0.15F};
};

struct RankedAsset {
    std::string asset_id;
    float priority{};
    bool protected_asset{};
};

[[nodiscard]] std::vector<RankedAsset> rank_assets(
    std::span<const AssetFeatures> assets,
    RankingPolicy policy = {});

}  // namespace pms

