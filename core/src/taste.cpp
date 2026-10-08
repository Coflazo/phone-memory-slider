#include "pms/taste.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <deque>
#include <iterator>
#include <numeric>
#include <random>
#include <stdexcept>
#include <utility>

#include "logistic.hpp"

namespace pms {
namespace {

double dot(std::span<const double> a, std::span<const double> b) {
    return std::inner_product(a.begin(), a.end(), b.begin(), 0.0);
}

void axpy(const double a, std::span<const double> x, std::span<double> y) {  // y += a x
    for (std::size_t j = 0; j < y.size(); ++j) y[j] += a * x[j];
}

double max_abs(std::span<const float> v) {
    double m = 0.0;
    for (const float x : v) m = std::max(m, double{std::abs(x)});
    return m;
}

}  // namespace

float TasteModel::logit(const std::span<const float> x) const noexcept {
    if (x.size() != weights.size()) return 0.0F;
    return std::inner_product(x.begin(), x.end(), weights.begin(), 0.0F) + bias;
}

float TasteModel::logit(const EmbeddingMatrix& m, const std::size_t row) const noexcept {
    if (row >= m.rows() || m.dim() != weights.size()) return 0.0F;
    return m.dot(row, weights) + bias;
}

// f(w,b) = sum_i [ log(1 + e^{z_i}) - y_i z_i ] + l2/2 |w|^2,  z_i = w.x_i + b
// grad_w = X^T (sigmoid(z) - y) + l2 w,  grad_b = sum_i (sigmoid(z_i) - y_i)
double taste_objective(std::span<const float> rows, std::size_t dim, std::span<const float> y,
                       std::span<const float> w, float b, float l2, std::span<float> grad_w, float& grad_b) {
    const std::size_t n = y.size();
    if (rows.size() != n * dim || w.size() != dim || grad_w.size() != dim)
        throw std::invalid_argument("taste_objective: rows, w and grad_w must hold n * dim, dim and dim values");
    double f = 0.0, gb = 0.0;
    std::vector<double> gw(dim, 0.0);
    for (std::size_t i = 0; i < n; ++i) {
        const float* xi = rows.data() + i * dim;
        double z = b;
        for (std::size_t j = 0; j < dim; ++j) z += double(w[j]) * xi[j];
        const auto [softplus, sigmoid] = detail::logistic(z);
        f += softplus - y[i] * z;
        const double r = sigmoid - y[i];
        gb += r;
        for (std::size_t j = 0; j < dim; ++j) gw[j] += r * xi[j];
    }
    for (std::size_t j = 0; j < dim; ++j) {
        f += 0.5 * l2 * double(w[j]) * w[j];
        grad_w[j] = static_cast<float>(gw[j] + double(l2) * w[j]);
    }
    grad_b = static_cast<float>(gb);
    return f;
}

TasteModel train_taste(const EmbeddingMatrix& x, const std::span<const std::size_t> positives,
                       const std::span<const std::size_t> negatives, const std::span<const std::size_t> unlabeled,
                       const TasteConfig& config, TasteReport* const report) {
    constexpr std::size_t history_size = 10;  // L-BFGS memory
    constexpr double c1 = 1e-4;               // Armijo: accept a step that achieves 1e-4 of the predicted decrease
    constexpr int max_halvings = 40;

    if (!std::isfinite(config.l2) || config.l2 < 0.0F || !std::isfinite(config.tolerance) || config.tolerance < 0.0F ||
        config.max_iterations <= 0)
        throw std::invalid_argument("train_taste: l2 and tolerance must be finite and >= 0, max_iterations > 0");

    // Each row sits in at most one list. A repeat within a list is dropped (first occurrence kept, order kept), so
    // lists without repeats train exactly as given.
    std::vector<std::uint8_t> list_of(x.rows(), 0);  // 0: unlisted, else 1 + the list's number
    std::array<std::vector<std::size_t>, 3> lists;
    const std::array given{positives, negatives, unlabeled};
    for (std::size_t k = 0; k < given.size(); ++k)
        for (const std::size_t row : given[k]) {
            if (row >= x.rows()) throw std::invalid_argument("train_taste: a row is past the matrix");
            if (list_of[row] == 0) {
                list_of[row] = static_cast<std::uint8_t>(k + 1);
                lists[k].push_back(row);
            } else if (list_of[row] != k + 1) {
                throw std::invalid_argument("train_taste: a row sits in two lists");
            }
        }
    const auto& [pos, neg, unl] = lists;

    // y = 0 rows: every negative, then a seeded, order-keeping sample of at most unlabeled_cap unlabeled rows.
    std::vector<std::size_t> zeros(neg);
    std::mt19937_64 rng(config.seed);
    std::ranges::sample(unl, std::back_inserter(zeros), static_cast<std::ptrdiff_t>(std::min(config.unlabeled_cap, unl.size())), rng);
    if (report != nullptr)
        *report = TasteReport{.loss = {}, .positives = pos.size(), .negatives = neg.size(),
                              .unlabeled_used = zeros.size() - neg.size(), .stop = TasteStop::Untrainable, .trained = false};

    const std::size_t dim = x.dim();
    TasteModel model{std::vector<float>(dim, 0.0F), 0.0F};
    if (pos.empty() || zeros.empty()) return model;

    // Dense float rows, widened from fp16 once: positives (y = 1), then zeros (y = 0).
    const std::size_t n = pos.size() + zeros.size();
    std::vector<float> rows(n * dim), y(n, 0.0F);
    std::fill_n(y.begin(), pos.size(), 1.0F);
    std::size_t i = 0;
    for (const auto list : {std::span<const std::size_t>(pos), std::span<const std::size_t>(zeros)})
        for (const std::size_t row : list) x.copy_row(row, std::span(rows).subspan(dim * i++, dim));

    // L-BFGS over theta = [w..., b]. theta and its gradient are floats, the model's own precision, so every
    // point evaluated is exactly a model we could return; directions and curvature pairs are doubles.
    std::vector<float> theta(dim + 1, 0.0F), grad(dim + 1), trial(dim + 1), trial_grad(dim + 1);
    const auto objective = [&](const std::vector<float>& t, std::vector<float>& g) {
        return taste_objective(rows, dim, y, std::span(t).first(dim), t[dim], config.l2, std::span(g).first(dim), g[dim]);
    };
    struct Curvature {
        std::vector<double> s, y;  // step and gradient change
        double rho;                // 1 / s.y
    };
    std::deque<Curvature> history;  // oldest first
    std::vector<double> d(dim + 1), alpha(history_size);

    // Two-loop recursion: d = -H grad, with H0 = (s.y / y.y) I from the newest pair, or 1 / |grad| without history
    // (a unit steepest-descent step). Returns the slope grad.d.
    const auto direction = [&] {
        std::ranges::copy(grad, d.begin());
        for (std::size_t k = history.size(); k-- > 0;) {
            alpha[k] = history[k].rho * dot(history[k].s, d);
            axpy(-alpha[k], history[k].y, d);
        }
        const double h0 = history.empty() ? 1.0 / std::sqrt(dot(d, d))
                                          : dot(history.back().s, history.back().y) / dot(history.back().y, history.back().y);
        for (double& v : d) v *= h0;
        for (std::size_t k = 0; k < history.size(); ++k) {
            const double beta = history[k].rho * dot(history[k].y, d);
            axpy(alpha[k] - beta, history[k].s, d);
        }
        for (double& v : d) v = -v;
        return std::inner_product(grad.begin(), grad.end(), d.begin(), 0.0);
    };

    double f = objective(theta, grad);
    std::vector<float> loss;
    const auto record = [&] {
        loss.push_back(static_cast<float>(f));
        if (config.on_iteration) config.on_iteration(static_cast<int>(loss.size() - 1), loss.back());
    };
    record();
    TasteStop stop{};
    for (int iteration = 0;; ++iteration) {
        if (max_abs(grad) < config.tolerance * std::max(1.0, std::abs(f))) {
            stop = TasteStop::Converged;
            break;
        }
        if (iteration == config.max_iterations) {
            stop = TasteStop::MaxIterations;
            break;
        }
        double slope = direction();
        if (!(slope < 0.0)) {  // H is positive definite, so only rounding or a NaN gets here: retry along -grad once
            history.clear();
            slope = direction();
        }
        if (!(slope < 0.0)) {  // not even steepest descent descends: the gradient is zero (or not finite)
            stop = TasteStop::NoDescent;
            break;
        }

        // Backtracking line search: halve the step until the loss falls by at least c1 * step * slope. Strict <,
        // because once c1 * step * slope is below f's rounding, <= would accept a step that does not move theta.
        double f_trial = 0.0;
        int halvings = 0;
        for (double step = 1.0; halvings <= max_halvings; step /= 2, ++halvings) {
            for (std::size_t j = 0; j <= dim; ++j) trial[j] = static_cast<float>(theta[j] + step * d[j]);
            f_trial = objective(trial, trial_grad);
            if (f_trial < f + c1 * step * slope) break;  // a NaN loss never passes
        }
        if (halvings > max_halvings) {  // no step lowers the loss: theta is the best point found
            stop = TasteStop::LineSearchFailed;
            break;
        }

        // Keep the pair only when s.y is clearly positive (curvature guard), so H stays positive definite.
        Curvature pair{std::vector<double>(dim + 1), std::vector<double>(dim + 1), 0.0};
        for (std::size_t j = 0; j <= dim; ++j) {
            pair.s[j] = double{trial[j]} - theta[j];
            pair.y[j] = double{trial_grad[j]} - grad[j];
        }
        const double sy = dot(pair.s, pair.y);
        if (sy > 1e-10 * std::sqrt(dot(pair.s, pair.s)) * std::sqrt(dot(pair.y, pair.y))) {
            pair.rho = 1.0 / sy;
            if (history.size() == history_size) history.pop_front();
            history.push_back(std::move(pair));
        }
        theta.swap(trial);
        grad.swap(trial_grad);
        f = f_trial;
        record();
    }

    if (report != nullptr) {
        report->loss = std::move(loss);
        report->stop = stop;
        report->trained = true;
    }
    model.weights.assign(theta.begin(), theta.begin() + static_cast<std::ptrdiff_t>(dim));
    model.bias = theta[dim];
    return model;
}

}  // namespace pms
