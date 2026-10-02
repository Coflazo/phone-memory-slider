#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "pms/ranking.hpp"

namespace pms {

[[nodiscard]] std::vector<AssetFeatures> make_fixture_catalog(
    std::size_t count,
    std::uint64_t seed);

}  // namespace pms

