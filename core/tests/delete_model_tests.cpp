#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <random>
#include <stdexcept>
#include <utility>
#include <vector>
#include "pms/delete_model.hpp"
#include "test_harness.hpp"

namespace {
template <typename Function>
bool throws_invalid_argument(Function&& function) {
    try {
        std::forward<Function>(function)();
    } catch (const std::invalid_argument&) {
        return true;
    }
    return false;
}
}  // namespace

void run_delete_model_tests() {
    using namespace pms;
    const auto prior = default_delete_model();
    DeleteFeatures dup{}; dup[DupNonBest] = 1.0F;
    DeleteFeatures doc{}; doc[DocumentLike] = 1.0F;
    pms_check(prior.probability(dup) > prior.probability(DeleteFeatures{}), "non-best duplicates lean delete at cold start");
    pms_check(prior.probability(doc) < prior.probability(DeleteFeatures{}), "documents lean keep at cold start");

    // Zero labels: fit returns the prior exactly.
    const auto same = fit_delete_model({}, {}, prior);
    for (std::size_t j = 0; j < delete_feature_count; ++j) pms_check(std::abs(same.beta[j] - prior.beta[j]) < 1e-6F, "no data keeps the prior");

    // A user who keeps every video: labels pull IsVideo negative.
    std::vector<DeleteFeatures> x; std::vector<std::uint8_t> y;
    std::mt19937 rng(5);
    for (int i = 0; i < 200; ++i) {
        DeleteFeatures f{}; f[IsVideo] = (i % 2) ? 1.0F : 0.0F; f[BlurZ] = std::normal_distribution<float>()(rng);
        x.push_back(f); y.push_back(f[IsVideo] > 0 ? 0 : static_cast<std::uint8_t>(i % 4 == 0));
    }
    const auto fit = fit_delete_model(x, y, prior);
    pms_check(fit.beta[IsVideo] < prior.beta[IsVideo] - 0.5F, "labels move a weight away from the prior");
    pms_check(std::isfinite(fit.bias), "fit stays finite");

    const auto z = fit_zscore(std::vector<float>{2, 2, 2});
    pms_check(std::abs(z(2.0F)) < 1e-6F && std::isfinite(z(3.0F)), "constant feature does not divide by zero");

    // The cold-start weights and bias are the documented ones; no labels gives the prior bit for bit.
    const DeleteModel expected{{1.2F, 0.8F, 0.6F, 2.0F, 0.9F, 0.5F, 0.2F, -1.0F}, -1.5F};
    pms_check(prior.beta == expected.beta && prior.bias == expected.bias, "cold start weights");
    pms_check(same.beta == prior.beta && same.bias == prior.bias, "no labels gives the prior exactly");

    // The fit is the MAP optimum: the gradient X^T (p - y) + (theta - theta0) / sigma^2 vanishes at it, up to the
    // float rounding of theta. p is recomputed in double here so only that rounding remains.
    const auto max_map_gradient = [&](const DeleteModel& m, const std::vector<DeleteFeatures>& xs,
                                      const std::vector<std::uint8_t>& ys, const float sigma) {
        std::array<double, delete_feature_count + 1> g{};
        for (std::size_t i = 0; i < xs.size(); ++i) {
            double z = m.bias;
            for (std::size_t j = 0; j < delete_feature_count; ++j) z += double{m.beta[j]} * xs[i][j];
            const double r = 1.0 / (1.0 + std::exp(-z)) - ys[i];
            for (std::size_t j = 0; j < delete_feature_count; ++j) g[j] += r * xs[i][j];
            g[delete_feature_count] += r;
        }
        const double inv_var = 1.0 / (double{sigma} * sigma);
        for (std::size_t j = 0; j < delete_feature_count; ++j) g[j] += (double{m.beta[j]} - prior.beta[j]) * inv_var;
        g[delete_feature_count] += (double{m.bias} - prior.bias) * inv_var;
        return std::ranges::max(g, {}, [](const double v) { return std::abs(v); });
    };
    for (const float sigma : {1.0F, 0.3F}) {
        const auto m = fit_delete_model(x, y, prior, sigma);
        pms_check(std::abs(max_map_gradient(m, x, y, sigma)) < 1e-3, "the fit is a stationary point of the MAP objective");
        pms_check(m.beta[ExposureZ] == prior.beta[ExposureZ], "a feature that is always 0 keeps its prior weight");
    }
    const auto tight = fit_delete_model(x, y, prior, 0.3F);
    pms_check(std::abs(tight.beta[IsVideo] - prior.beta[IsVideo]) < std::abs(fit.beta[IsVideo] - prior.beta[IsVideo]),
              "a tighter prior moves the weights less");

    // Labels that contradict the prior on large features, with a weak prior: this user deletes the items the
    // taste model likes. Undamped Newton overshoots here (weights near -4e6 and gradient ~400 after 25 steps); the damped one
    // converges (NegTaste weight near -62).
    std::vector<DeleteFeatures> sx; std::vector<std::uint8_t> sy;
    for (int i = 0; i < 200; ++i) {
        DeleteFeatures f{}; f[NegTaste] = -8.0F + 16.0F * static_cast<float>(i) / 199.0F; f[DupNonBest] = 1.0F;
        sx.push_back(f); sy.push_back(static_cast<std::uint8_t>(f[NegTaste] < 0.0F));
    }
    const auto flipped = fit_delete_model(sx, sy, prior, 100.0F);
    bool finite = std::isfinite(flipped.bias);
    for (const float b : flipped.beta) finite = finite && std::isfinite(b);
    pms_check(finite, "contradicting labels with a weak prior stay finite");
    pms_check(flipped.probability(sx.front()) > 0.99F && flipped.probability(sx.back()) < 0.01F, "contradicting labels are learned");
    pms_check(std::abs(max_map_gradient(flipped, sx, sy, 100.0F)) < 1e-2, "contradicting labels converge to the MAP optimum");

    // Review Focus 2: with no taste model (NegTaste 0 everywhere) the cold start still ranks by evidence:
    // non-best duplicates and blurry items above clean ones, documents below, and scores are not all equal.
    DeleteFeatures clean{}, blurry{}, document{};
    blurry[BlurZ] = 2.0F;
    document[DocumentLike] = 1.0F;
    std::vector<std::pair<float, int>> deck;  // (probability, 0 duplicate / 1 blurry / 2 clean / 3 document)
    for (const auto& [features, kind] : {std::pair{document, 3}, std::pair{clean, 2}, std::pair{blurry, 1}, std::pair{dup, 0}}) {
        pms_check(features[NegTaste] == 0.0F, "no taste model means NegTaste 0");
        deck.emplace_back(prior.probability(features), kind);
    }
    std::ranges::sort(deck, std::ranges::greater{});
    for (std::size_t k = 0; k < deck.size(); ++k)
        pms_check(deck[k].second == static_cast<int>(k), "evidence alone orders duplicates, blurry, clean, documents");
    pms_check(deck.front().first > deck.back().first + 0.3F, "evidence alone spreads the scores");

    // probability is a bounded sigmoid even for extreme logits.
    DeleteFeatures extreme{}; extreme[NegTaste] = 1e30F;
    pms_check(prior.probability(extreme) == 1.0F && std::abs(prior.logit(dup) - 0.5F) < 1e-6F, "logit and probability agree");
    extreme[NegTaste] = -1e30F;
    pms_check(prior.probability(extreme) == 0.0F, "a very negative logit gives probability 0");

    // fit_zscore: mean and population stddev of the finite values; nothing finite gives {0, 1}.
    const float nan = std::numeric_limits<float>::quiet_NaN(), inf = std::numeric_limits<float>::infinity();
    const auto z13 = fit_zscore(std::vector<float>{1.0F, nan, 3.0F, inf});
    pms_check(std::abs(z13.mean - 2.0F) < 1e-6F && std::abs(z13.stddev - 1.0F) < 1e-6F && std::abs(z13(4.0F) - 2.0F) < 1e-6F,
              "z-score skips non-finite values");
    const auto none = fit_zscore({});
    pms_check(none.mean == 0.0F && none.stddev == 1.0F, "no values gives the identity z-score");
    const auto only_nan = fit_zscore(std::vector<float>{nan});
    pms_check(only_nan.mean == 0.0F && only_nan.stddev == 1.0F, "no finite values gives the identity z-score");
    pms_check(std::abs(z.stddev - 1e-3F) < 1e-9F, "the stddev floor is 1e-3");

    // Bad input throws.
    const std::vector<std::uint8_t> two_labels{0, 1}, bad_label{2};
    const std::vector<DeleteFeatures> one_row{DeleteFeatures{}};
    std::vector<DeleteFeatures> nan_row{DeleteFeatures{}};
    nan_row[0][BlurZ] = nan;
    const std::vector<std::uint8_t> one_label{1};
    DeleteModel bad_prior = prior;
    bad_prior.beta[IsVideo] = nan;
    pms_check(throws_invalid_argument([&] { (void)fit_delete_model(one_row, two_labels, prior); }), "sizes must match");
    pms_check(throws_invalid_argument([&] { (void)fit_delete_model(one_row, bad_label, prior); }), "labels must be 0 or 1");
    pms_check(throws_invalid_argument([&] { (void)fit_delete_model(nan_row, one_label, prior); }), "features must be finite");
    pms_check(throws_invalid_argument([&] { (void)fit_delete_model(one_row, one_label, bad_prior); }), "the prior must be finite");
    for (const float sigma : {0.0F, -1.0F, nan, inf})
        pms_check(throws_invalid_argument([&] { (void)fit_delete_model(one_row, one_label, prior, sigma); }), "prior_sigma must be finite and > 0");
    pms_check(throws_invalid_argument([&] { (void)fit_delete_model({}, {}, prior, 0.0F); }), "prior_sigma is checked even without labels");
}
