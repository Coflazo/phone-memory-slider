#include <chrono>
#include <cstddef>

#include "pms/fixture_catalog.hpp"
#include "pms/ranking.hpp"
#include "test_harness.hpp"

void run_fixture_catalog_tests() {
    const auto first = pms::make_fixture_catalog(64, 42);
    const auto same = pms::make_fixture_catalog(64, 42);
    const auto different = pms::make_fixture_catalog(64, 43);

    pms_check(first.size() == 64, "fixture generator returns the requested count");
    pms_check(first.front().asset_id == same.front().asset_id, "fixture IDs are reproducible");
    pms_check(first.front().cleanup_confidence == same.front().cleanup_confidence, "fixture features are reproducible");
    pms_check(first.front().cleanup_confidence != different.front().cleanup_confidence, "seed changes fixture features");

    const auto large = pms::make_fixture_catalog(250'000, 20261002);
    const auto started = std::chrono::steady_clock::now();
    const auto ranked = pms::rank_assets(large);
    const auto elapsed = std::chrono::steady_clock::now() - started;

    pms_check(ranked.size() == 250'000, "large ranking preserves the catalog");
    pms_check(elapsed < std::chrono::seconds{2}, "250k ranking completes under two seconds");
}

