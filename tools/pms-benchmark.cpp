#include <chrono>
#include <cstddef>
#include <iostream>

#include "pms/fixture_catalog.hpp"
#include "pms/ranking.hpp"

int main() {
    constexpr std::size_t asset_count = 250'000;
    const auto catalog = pms::make_fixture_catalog(asset_count, 20261002);
    const auto started = std::chrono::steady_clock::now();
    const auto ranked = pms::rank_assets(catalog);
    const auto elapsed = std::chrono::steady_clock::now() - started;
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
    const auto estimated_bytes = catalog.capacity() * sizeof(pms::AssetFeatures) +
                                 ranked.capacity() * sizeof(pms::RankedAsset);

    std::cout << "{\"assets\":" << asset_count << ",\"rank_ms\":" << elapsed_ms
              << ",\"estimated_bytes\":" << estimated_bytes << "}\n";
    return ranked.size() == asset_count && elapsed < std::chrono::seconds{2} ? 0 : 1;
}

