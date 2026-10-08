#pragma once

#include <algorithm>
#include <cmath>

namespace pms::detail {

// softplus(z) = log(1 + e^z) and sigmoid(z), exact for any finite z: e = e^{-|z|} lies in (0, 1], so nothing
// overflows. Shared by the taste head and the delete model.
struct Logistic {
    double softplus;
    double sigmoid;
};

[[nodiscard]] inline Logistic logistic(const double z) noexcept {
    const double e = std::exp(-std::abs(z));
    return {std::max(z, 0.0) + std::log1p(e), (z >= 0 ? 1.0 : e) / (1.0 + e)};
}

}  // namespace pms::detail
