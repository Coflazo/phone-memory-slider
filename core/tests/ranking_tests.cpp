#include <cmath>
#include <limits>
#include <vector>

#include "pms/ranking.hpp"
#include "test_harness.hpp"

using pms::AssetFeatures;
using pms::RankingPolicy;

void run_ranking_tests() {
    {
        const std::vector assets{
            AssetFeatures{"favorite", 1.0F, 0.0F, 1.0F, true, false},
            AssetFeatures{"kept", 1.0F, 0.0F, 1.0F, false, true},
            AssetFeatures{"candidate", 0.8F, 0.1F, 0.6F, false, false},
        };
        const auto ranked = pms::rank_assets(assets, RankingPolicy{});
        pms_check(ranked.size() == 3, "ranking preserves every input asset");
        pms_check(ranked.front().asset_id == "candidate", "protected assets sort behind candidates");
        pms_check(ranked[1].protected_asset && ranked[2].protected_asset, "favorites and keeps are protected");
    }

    {
        const std::vector assets{
            AssetFeatures{"nan", std::numeric_limits<float>::quiet_NaN(), 0.2F, 0.4F, false, false},
            AssetFeatures{"finite", 0.4F, 0.4F, 0.4F, false, false},
        };
        const auto ranked = pms::rank_assets(assets, RankingPolicy{});
        pms_check(ranked.size() == 2, "non-finite inputs remain representable");
        pms_check(std::isfinite(ranked[0].priority) && std::isfinite(ranked[1].priority), "ranking sanitizes NaN");
    }

    {
        const std::vector assets{
            AssetFeatures{"zeta", 0.5F, 0.5F, 0.5F, false, false},
            AssetFeatures{"alpha", 0.5F, 0.5F, 0.5F, false, false},
        };
        const auto ranked = pms::rank_assets(assets, RankingPolicy{});
        pms_check(ranked[0].asset_id == "alpha" && ranked[1].asset_id == "zeta", "ties use stable asset IDs");
    }
}
