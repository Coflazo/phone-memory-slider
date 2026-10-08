#include "pms/delete_model.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <stdexcept>

#include "logistic.hpp"

namespace pms {
namespace {

constexpr std::size_t theta_size = delete_feature_count + 1;  // [beta..., bias]
using Vector = std::array<double, theta_size>;
using Matrix = std::array<Vector, theta_size>;

double dot(const Vector& a, const Vector& b) { return std::inner_product(a.begin(), a.end(), b.begin(), 0.0); }

// MAP objective f = sum_i [softplus(z_i) - y_i z_i] + |theta - theta0|^2 / (2 sigma^2), z_i = beta.x_i + bias.
// With grad and hessian: grad = X~^T (p - y) + (theta - theta0) / sigma^2, hessian = X~^T diag(p(1-p)) X~ + I / sigma^2,
// where X~ appends a 1 to every row for the bias.
double objective(std::span<const DeleteFeatures> x, std::span<const std::uint8_t> y, const Vector& theta,
                 const Vector& theta0, const double inv_var, Vector* const grad = nullptr, Matrix* const hessian = nullptr) {
    double f = 0.0;
    if (grad != nullptr) *grad = {};
    if (hessian != nullptr) *hessian = {};
    for (std::size_t i = 0; i < x.size(); ++i) {
        Vector row{};
        std::ranges::copy(x[i], row.begin());
        row[delete_feature_count] = 1.0;
        const double z = dot(theta, row);
        const auto [softplus, p] = detail::logistic(z);
        f += softplus - y[i] * z;
        if (grad != nullptr)
            for (std::size_t j = 0; j < theta_size; ++j) (*grad)[j] += (p - y[i]) * row[j];
        if (hessian != nullptr)
            for (std::size_t j = 0; j < theta_size; ++j)
                for (std::size_t k = 0; k <= j; ++k) (*hessian)[j][k] += p * (1.0 - p) * row[j] * row[k];
    }
    for (std::size_t j = 0; j < theta_size; ++j) {
        const double d = theta[j] - theta0[j];
        f += 0.5 * inv_var * d * d;
        if (grad != nullptr) (*grad)[j] += inv_var * d;
        if (hessian != nullptr) (*hessian)[j][j] += inv_var;
    }
    return f;
}

// Solves H s = g for symmetric positive definite H (lower triangle used) by Cholesky. False if H is not
// numerically positive definite.
bool cholesky_solve(Matrix h, const Vector& g, Vector& s) {
    for (std::size_t j = 0; j < theta_size; ++j) {  // h's lower triangle becomes L, H = L L^T
        double d = h[j][j];
        for (std::size_t k = 0; k < j; ++k) d -= h[j][k] * h[j][k];
        if (!(d > 0.0)) return false;
        h[j][j] = std::sqrt(d);
        for (std::size_t i = j + 1; i < theta_size; ++i) {
            double v = h[i][j];
            for (std::size_t k = 0; k < j; ++k) v -= h[i][k] * h[j][k];
            h[i][j] = v / h[j][j];
        }
    }
    for (std::size_t i = 0; i < theta_size; ++i) {  // L u = g
        double v = g[i];
        for (std::size_t k = 0; k < i; ++k) v -= h[i][k] * s[k];
        s[i] = v / h[i][i];
    }
    for (std::size_t i = theta_size; i-- > 0;) {  // L^T s = u
        double v = s[i];
        for (std::size_t k = i + 1; k < theta_size; ++k) v -= h[k][i] * s[k];
        s[i] = v / h[i][i];
    }
    return true;
}

}  // namespace

float DeleteModel::logit(const DeleteFeatures& x) const noexcept {
    return std::inner_product(x.begin(), x.end(), beta.begin(), bias);
}

float DeleteModel::probability(const DeleteFeatures& x) const noexcept {
    return static_cast<float>(detail::logistic(logit(x)).sigmoid);
}

DeleteModel default_delete_model() noexcept {
    return {{1.2F, 0.8F, 0.6F, 2.0F, 0.9F, 0.5F, 0.2F, -1.0F}, -1.5F};
}

DeleteModel fit_delete_model(const std::span<const DeleteFeatures> x, const std::span<const std::uint8_t> deleted,
                             const DeleteModel& prior, const float prior_sigma) {
    constexpr int max_iterations = 25;
    constexpr double min_step = 1e-8;
    constexpr double c1 = 1e-4;  // Armijo: a damped step must achieve 1e-4 of the predicted decrease
    constexpr int max_halvings = 30;

    const auto finite = [](const float v) { return std::isfinite(v); };
    if (!std::isfinite(prior_sigma) || !(prior_sigma > 0.0F))
        throw std::invalid_argument("fit_delete_model: prior_sigma must be finite and > 0");
    if (!std::ranges::all_of(prior.beta, finite) || !std::isfinite(prior.bias))
        throw std::invalid_argument("fit_delete_model: the prior must be finite");
    if (x.size() != deleted.size()) throw std::invalid_argument("fit_delete_model: x and deleted differ in size");
    for (std::size_t i = 0; i < x.size(); ++i)
        if (deleted[i] > 1 || !std::ranges::all_of(x[i], finite))
            throw std::invalid_argument("fit_delete_model: labels must be 0 or 1 and features finite");
    if (x.empty()) return prior;

    Vector theta0{};
    std::ranges::copy(prior.beta, theta0.begin());
    theta0[delete_feature_count] = prior.bias;
    const double inv_var = 1.0 / (double{prior_sigma} * prior_sigma);

    // Newton from the prior. The objective is strictly convex, but a full Newton step can overshoot when the
    // logistic terms saturate, so the step is halved until the objective falls (Armijo).
    Vector theta = theta0, grad{}, step{}, trial{};
    Matrix hessian{};
    for (int iteration = 0; iteration < max_iterations; ++iteration) {
        const double f = objective(x, deleted, theta, theta0, inv_var, &grad, &hessian);
        if (!cholesky_solve(hessian, grad, step)) break;
        const double norm = std::sqrt(dot(step, step));
        if (norm < min_step) {  // converged: take the last, tiny Newton step undamped
            for (std::size_t j = 0; j < theta_size; ++j) theta[j] -= step[j];
            break;
        }
        const double decrease = dot(grad, step);  // > 0: -step is a descent direction
        double t = 1.0;
        int halvings = 0;
        for (; halvings <= max_halvings; ++halvings, t /= 2) {
            for (std::size_t j = 0; j < theta_size; ++j) trial[j] = theta[j] - t * step[j];
            if (objective(x, deleted, trial, theta0, inv_var) <= f - c1 * t * decrease) break;
        }
        if (halvings > max_halvings) break;  // no step lowers the objective: theta is the best point found
        theta = trial;
        if (t * norm < min_step) break;
    }

    DeleteModel model;
    for (std::size_t j = 0; j < delete_feature_count; ++j) model.beta[j] = static_cast<float>(theta[j]);
    model.bias = static_cast<float>(theta[delete_feature_count]);
    return model;
}

float ZScore::operator()(const float v) const noexcept { return (v - mean) / stddev; }

ZScore fit_zscore(const std::span<const float> values) noexcept {
    double sum = 0.0;
    std::size_t n = 0;
    for (const float v : values)
        if (std::isfinite(v)) {
            sum += v;
            ++n;
        }
    if (n == 0) return {};
    const double mean = sum / static_cast<double>(n);
    double squares = 0.0;
    for (const float v : values)
        if (std::isfinite(v)) squares += (v - mean) * (v - mean);
    const double stddev = std::max(std::sqrt(squares / static_cast<double>(n)), 1e-3);
    return {static_cast<float>(mean), static_cast<float>(stddev)};
}

}  // namespace pms
