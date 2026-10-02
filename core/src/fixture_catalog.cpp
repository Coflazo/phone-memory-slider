#include "pms/fixture_catalog.hpp"

#include <string>

namespace pms {
namespace {

[[nodiscard]] std::uint64_t splitmix64(std::uint64_t& state) noexcept {
    auto value = (state += 0x9E3779B97F4A7C15ULL);
    value = (value ^ (value >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    value = (value ^ (value >> 27U)) * 0x94D049BB133111EBULL;
    return value ^ (value >> 31U);
}

[[nodiscard]] float unit_float(std::uint64_t& state) noexcept {
    constexpr auto denominator = static_cast<float>(1U << 24U);
    const auto upper = static_cast<std::uint32_t>(splitmix64(state) >> 40U);
    return static_cast<float>(upper) / denominator;
}

}  // namespace

std::vector<AssetFeatures> make_fixture_catalog(const std::size_t count, const std::uint64_t seed) {
    std::vector<AssetFeatures> assets;
    assets.reserve(count);
    auto state = seed;
    for (std::size_t index = 0; index < count; ++index) {
        assets.push_back(AssetFeatures{
            "fixture-" + std::to_string(index),
            unit_float(state),
            unit_float(state),
            unit_float(state),
            index % 97U == 0U,
            index % 131U == 0U,
        });
    }
    return assets;
}

}  // namespace pms

