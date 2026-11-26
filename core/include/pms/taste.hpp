#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <vector>

#include "pms/embeddings.hpp"

namespace pms {

// train_taste throws std::invalid_argument unless l2 and tolerance are finite and >= 0 and max_iterations > 0.
struct TasteConfig {
    float l2{1.0F};              // L2 strength on the weights (not the bias); scikit-learn's C is 1 / l2
    int max_iterations{200};     // L-BFGS iterations
    float tolerance{1e-6F};      // stop when max |gradient| < tolerance * max(1, |loss|)
    std::size_t unlabeled_cap{8000};
    std::uint64_t seed{0};       // picks the unlabeled subsample when there are more than unlabeled_cap
    // Live loss curve: called with (0, initial loss) and then (k, loss) after the k-th accepted iteration, the
    // same values TasteReport::loss collects. Called synchronously on the thread running train_taste, never
    // concurrently; marshal to a GUI thread yourself. Not called when train_taste returns the zero model. An
    // exception it throws propagates out of train_taste.
    std::function<void(int iteration, float loss)> on_iteration{};
};

// Why training stopped.
enum class TasteStop : std::uint8_t {
    Converged,         // max |gradient| fell below the tolerance
    MaxIterations,     // max_iterations accepted iterations ran
    LineSearchFailed,  // no step along the search direction lowered the loss (the float precision floor)
    NoDescent,         // even steepest descent (L-BFGS history cleared) gave no descent direction: zero gradient
    Untrainable,       // no positives or no rows labeled 0: train_taste returned the zero model
};

// A linear score on one embedding: logit = w.x + b, so taste probability = sigmoid(logit). A default or
// zero model scores everything 0. A span whose size is not weights.size(), or a missing row, also gives 0.
struct TasteModel {
    std::vector<float> weights;
    float bias{};
    [[nodiscard]] float logit(std::span<const float> x) const noexcept;
    [[nodiscard]] float logit(const EmbeddingMatrix& m, std::size_t row) const noexcept;
};

struct TasteReport {
    std::vector<float> loss;  // objective at the start, then after every L-BFGS iteration (never increases);
                              // empty when train_taste returned a zero model
    std::size_t positives{}, negatives{}, unlabeled_used{};  // distinct rows used
    TasteStop stop{TasteStop::Untrainable};
    bool trained{};  // false exactly when stop is Untrainable (the zero model)
};

// Objective: sum_i logloss(y_i, w.x_i + b) + (l2/2)||w||^2  (equals scikit-learn LogisticRegression(C = 1/l2)).
// y = 1 for positives (favorites, seeds, swipe keeps); y = 0 for negatives (swipe deletes) and for a seeded
// subsample of at most unlabeled_cap unlabeled rows. Returns a zero model (dim() zero weights, bias 0) when
// there are no positives or no rows labeled 0, since then the best bias is infinite.
// Every index must name an existing row. A row repeated within one list counts once; a row in two lists throws
// std::invalid_argument: a row cannot be both kept and deleted, or both a favorite and unlabeled.
// Deterministic: the same rows, lists, config and standard library give the same model bit for bit.
[[nodiscard]] TasteModel train_taste(const EmbeddingMatrix& x, std::span<const std::size_t> positives,
                                     std::span<const std::size_t> negatives, std::span<const std::size_t> unlabeled,
                                     const TasteConfig& config = {}, TasteReport* report = nullptr);

// For tests: objective value and gradient at (w, b) on dense float rows (y.size() rows of dim floats).
// Throws std::invalid_argument unless rows, w and grad_w hold y.size() * dim, dim and dim values.
[[nodiscard]] double taste_objective(std::span<const float> rows, std::size_t dim, std::span<const float> y,
                                     std::span<const float> w, float b, float l2, std::span<float> grad_w, float& grad_b);

}  // namespace pms
