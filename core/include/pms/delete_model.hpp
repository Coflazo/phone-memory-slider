#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace pms {

inline constexpr std::size_t delete_feature_count = 8;

// One row of evidence per item; a larger value always argues for deletion except DocumentLike.
//   NegTaste      -clip(taste_logit, -8, 8): disliked items are positive. 0 when there is no taste model (no
//                 favorites or seeds yet), so the ranking falls back to the evidence below.
//   BlurZ         blurriness as a z-score over the gallery (fit_zscore): higher is blurrier
//   ExposureZ     exposure badness as a z-score: higher is darker or more blown out
//   DupNonBest    1 when the item sits in a duplicate cluster and is not its best member, else 0
//   ScreenshotAge log(1 + age in days) for screenshots and screen recordings, 0 for everything else
//   LogBytesZ     log(file bytes) as a z-score: big files free more space
//   IsVideo       1 for videos, 0 for stills
//   DocumentLike  1 for receipts, tickets, IDs and other documents (kept but never favorited), else 0
enum DeleteFeature : std::size_t { NegTaste, BlurZ, ExposureZ, DupNonBest, ScreenshotAge, LogBytesZ, IsVideo, DocumentLike };
using DeleteFeatures = std::array<float, delete_feature_count>;

// P(delete | x) = sigmoid(beta.x + bias).
struct DeleteModel {
    std::array<float, delete_feature_count> beta{};
    float bias{};
    [[nodiscard]] float logit(const DeleteFeatures& x) const noexcept;
    [[nodiscard]] float probability(const DeleteFeatures& x) const noexcept;
};

// Cold start: beta = {1.2, 0.8, 0.6, 2.0, 0.9, 0.5, 0.2, -1.0}, bias = -1.5 (documents lean keep; non-best
// duplicates lean delete).
[[nodiscard]] DeleteModel default_delete_model() noexcept;

// MAP fit on swipe labels (deleted[i] = 1 for a swipe delete, 0 for a keep):
//   minimize sum_i logloss(deleted_i, logit(x_i)) + |theta - theta0|^2 / (2 prior_sigma^2),
// theta = [beta, bias], theta0 = the prior's. Damped Newton (exact 9x9 Hessian, Cholesky solve, the step halved
// until the objective falls), at most 25 iterations, stopping once a step is shorter than 1e-8. If the Hessian is
// not numerically positive definite (the Cholesky solve fails, which only extreme features can cause), the fit stops
// and returns the current theta: the prior itself when that happens on the first step. No labels gives the prior
// exactly. Throws std::invalid_argument when x and deleted differ in size, a label is not 0 or 1, a
// feature or a prior weight is not finite, or prior_sigma is not finite and > 0.
[[nodiscard]] DeleteModel fit_delete_model(std::span<const DeleteFeatures> x, std::span<const std::uint8_t> deleted,
                                           const DeleteModel& prior, float prior_sigma = 1.0F);

// z-score with the mean and population standard deviation of the unlabeled set.
struct ZScore {
    float mean{}, stddev{1.0F};
    [[nodiscard]] float operator()(float v) const noexcept;
};
// Non-finite values are skipped; the standard deviation is floored at 1e-3, so a constant feature maps to 0
// instead of dividing by zero; no finite values gives {0, 1}.
[[nodiscard]] ZScore fit_zscore(std::span<const float> values) noexcept;

}  // namespace pms
