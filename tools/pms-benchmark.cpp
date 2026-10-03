#include <chrono>
#include <cstddef>
#include <iostream>
#include <vector>

#include "pms/fixture_catalog.hpp"
#include "pms/preference_model.hpp"
#include "pms/ranking.hpp"

int main() {
    constexpr std::size_t asset_count = 250'000;
    auto catalog = pms::make_fixture_catalog(asset_count, 20261002);
    std::vector<pms::PreferenceExample> examples;
    examples.reserve(asset_count);
    for (std::size_t index = 0; index < asset_count; ++index) {
        const auto first = static_cast<float>((index * 17U) % 101U) / 100.0F;
        const auto second = static_cast<float>((index * 31U) % 101U) / 100.0F;
        auto label = pms::PreferenceLabel::Unlabeled;
        if (index < 20) {
            label = pms::PreferenceLabel::Favorite;
        } else if (index < 50) {
            label = index % 2 == 0 ? pms::PreferenceLabel::Keep : pms::PreferenceLabel::Delete;
        }
        examples.push_back({pms::FixedEmbedding{first, second}, label});
    }

    const auto started = std::chrono::steady_clock::now();
    pms::PreferenceModel model;
    model.train(examples);
    for (std::size_t index = 0; index < catalog.size(); ++index) {
        catalog[index].keep_score = model.score(examples[index].embedding);
    }
    const auto ranked = pms::rank_assets(catalog);
    const auto elapsed = std::chrono::steady_clock::now() - started;
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
    const auto estimated_bytes = catalog.capacity() * sizeof(pms::AssetFeatures) +
                                 ranked.capacity() * sizeof(pms::RankedAsset) +
                                 examples.capacity() * sizeof(pms::PreferenceExample);

    std::cout << "{\"assets\":" << asset_count << ",\"model_and_rank_ms\":" << elapsed_ms
              << ",\"estimated_bytes\":" << estimated_bytes << "}\n";
    return ranked.size() == asset_count && model.enabled() && elapsed < std::chrono::seconds{2} &&
                   estimated_bytes < 1'500'000'000
               ? 0
               : 1;
}
