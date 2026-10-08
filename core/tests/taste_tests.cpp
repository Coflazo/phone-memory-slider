#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <fstream>
#include <random>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include "pms/taste.hpp"
#include "test_harness.hpp"

#ifndef PMS_TEST_FIXTURES
#error "PMS_TEST_FIXTURES must point at core/tests/fixtures"
#endif

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

void run_taste_tests() {
    // 1. Gradient check against central finite differences.
    std::mt19937 rng(3);
    std::normal_distribution<float> n;
    const std::size_t rows = 30, dim = 6;
    std::vector<float> x(rows * dim), y(rows), w(dim), g(dim);
    for (auto& v : x) v = n(rng);
    for (auto& v : y) v = n(rng) > 0 ? 1.0F : 0.0F;
    for (auto& v : w) v = 0.3F * n(rng);
    float b = 0.1F, gb = 0;
    (void)pms::taste_objective(x, dim, y, w, b, 1.0F, g, gb);
    for (std::size_t j = 0; j < dim; ++j) {
        auto wp = w, wm = w; wp[j] += 1e-3F; wm[j] -= 1e-3F;
        std::vector<float> tmp(dim); float tb = 0;
        const double num = (pms::taste_objective(x, dim, y, wp, b, 1.0F, tmp, tb) - pms::taste_objective(x, dim, y, wm, b, 1.0F, tmp, tb)) / 2e-3;
        pms_check(std::abs(num - g[j]) < 1e-2 * std::max(1.0, std::abs(num)), "analytic gradient matches finite differences");
    }

    // 2. Parity with scikit-learn on a fixed problem.
    std::ifstream f(std::string(PMS_TEST_FIXTURES) + "/taste_reference.bin", std::ios::binary);
    std::vector<float> blob(200 * 16 + 200 + 16 + 1);
    f.read(reinterpret_cast<char*>(blob.data()), static_cast<std::streamsize>(blob.size() * sizeof(float)));
    pms_check(static_cast<bool>(f), "fixture readable");
    pms::EmbeddingMatrix m(16);
    std::vector<std::size_t> pos, neg;
    for (std::size_t i = 0; i < 200; ++i) {
        m.append(std::span<const float>(blob.data() + i * 16, 16));
        (blob[3200 + i] > 0.5F ? pos : neg).push_back(i);
    }
    pms::TasteReport report;
    const auto model = pms::train_taste(m, pos, neg, {}, {}, &report);
    for (std::size_t j = 0; j < 16; ++j)
        pms_check(std::abs(model.weights[j] - blob[3400 + j]) < 2e-2F, "weights match scikit-learn");
    pms_check(std::abs(model.bias - blob[3416]) < 2e-2F, "bias matches scikit-learn");
    pms_check(report.loss.size() >= 2 && report.loss.back() <= report.loss.front(), "loss decreases");

    // 3. No positives -> neutral model; unlabeled cap respected and seeded.
    const auto empty = pms::train_taste(m, {}, neg, {}, {});
    pms_check(std::abs(empty.logit(m, 0)) < 1e-6F, "no positives gives a neutral model");
    std::vector<std::size_t> unl(neg.begin(), neg.end());
    pms::TasteReport capped;
    (void)pms::train_taste(m, pos, {}, unl, pms::TasteConfig{.unlabeled_cap = 10}, &capped);
    pms_check(capped.unlabeled_used == 10, "unlabeled subsample respects the cap");

    // 4. The report, the loss curve and the logit overloads.
    pms_check(report.positives == pos.size() && report.negatives == neg.size() && report.unlabeled_used == 0, "report counts the rows");
    pms_check(std::ranges::is_sorted(report.loss, std::ranges::greater{}), "every accepted step lowers the loss");
    pms::TasteReport exhaustive;
    (void)pms::train_taste(m, pos, neg, {}, pms::TasteConfig{.tolerance = 0.0F}, &exhaustive);
    pms_check(exhaustive.loss.size() < 201, "with tolerance 0 training stops when no step lowers the loss");
    std::vector<float> row(16);
    m.copy_row(3, row);
    pms_check(std::abs(model.logit(row) - model.logit(m, 3)) < 1e-5F, "logit of a stored row matches logit of its floats");
    pms_check(model.logit(std::vector<float>(15, 1.0F)) == 0.0F && model.logit(m, 200) == 0.0F, "bad logit arguments give 0");
    pms_check(pms::train_taste(m, pos, {}, {}).logit(m, 0) == 0.0F, "no rows labeled 0 gives a neutral model");
    pms::TasteReport untrained;
    (void)pms::train_taste(m, {}, neg, {}, {}, &untrained);
    pms_check(untrained.loss.empty() && untrained.positives == 0 && untrained.negatives == neg.size(), "a zero model still fills the report");

    // 5. Same seed, same model bit for bit; another seed draws another subsample.
    const auto seeded = [&](std::uint64_t seed) { return pms::train_taste(m, pos, {}, unl, pms::TasteConfig{.unlabeled_cap = 50, .seed = seed}); };
    const auto first = seeded(7), again = seeded(7), other = seeded(8);
    pms_check(first.weights == again.weights && first.bias == again.bias, "the same seed gives the same model");
    pms_check(first.weights != other.weights, "another seed gives another subsample");

    // 6. softplus and sigmoid stay exact at |z| = 1e4 (a naive log(1 + e^z) is inf there).
    const std::vector<float> two_rows{1.0F, -1.0F}, far{1e4F};
    std::vector<float> g1(1);
    float g1b = 1.0F;
    const double right = pms::taste_objective(two_rows, 1, std::vector<float>{1.0F, 0.0F}, far, 0.0F, 0.0F, g1, g1b);
    pms_check(right == 0.0 && g1[0] == 0.0F && g1b == 0.0F, "confident and right costs nothing");
    const double wrong = pms::taste_objective(two_rows, 1, std::vector<float>{0.0F, 1.0F}, far, 0.0F, 0.0F, g1, g1b);
    pms_check(wrong == 2e4 && g1[0] == 2.0F && g1b == 0.0F, "confident and wrong costs |z| per row");

    // 7. Each row may sit in one list only, and must exist.
    const std::vector<std::size_t> row0{0}, past_end{200};
    pms_check(throws_invalid_argument([&] { (void)pms::train_taste(m, row0, row0, {}); }), "a row both kept and deleted throws");
    pms_check(throws_invalid_argument([&] { (void)pms::train_taste(m, row0, {}, row0); }), "a positive that is also unlabeled throws");
    pms_check(throws_invalid_argument([&] { (void)pms::train_taste(m, past_end, neg, {}); }), "a row past the matrix throws");
    pms_check(throws_invalid_argument([&] { (void)pms::taste_objective(x, dim + 1, y, w, b, 1.0F, g, gb); }), "taste_objective checks its sizes");
}
