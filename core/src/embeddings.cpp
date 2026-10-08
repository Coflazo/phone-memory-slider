#include "pms/embeddings.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace pms {

std::uint16_t half_from_float(const float value) noexcept {
    const auto bits = std::bit_cast<std::uint32_t>(value);
    const auto sign = static_cast<std::uint16_t>((bits >> 16) & 0x8000U);
    const std::uint32_t magnitude = bits & 0x7FFF'FFFFU;
    if (magnitude > 0x7F80'0000U)  // NaN: keep the top 10 payload bits, set the quiet bit so it cannot become inf
        return static_cast<std::uint16_t>(sign | 0x7E00U | ((magnitude >> 13) & 0x03FFU));
    std::uint32_t half{}, dropped{}, halfway{};
    if (magnitude >= 0x3880'0000U) {  // >= 2^-14, a normal half (or overflow): rebias 127 -> 15, keep 10 bits
        half = (magnitude - 0x3800'0000U) >> 13;
        dropped = magnitude & 0x1FFFU;
        halfway = 0x1000U;
    } else if (magnitude >= 0x3300'0000U) {  // [2^-25, 2^-14): subnormal half, mantissa * 2^-24
        const std::uint32_t exponent = magnitude >> 23, significand = (magnitude & 0x7F'FFFFU) | 0x80'0000U;
        const std::uint32_t shift = 126 - exponent;  // 14..24
        half = significand >> shift;
        dropped = significand & ((1U << shift) - 1);
        halfway = 1U << (shift - 1);
    } else {
        return sign;  // below 2^-25, including float subnormals: rounds to a signed zero
    }
    // Round to nearest, ties to even. A carry out of the mantissa bumps the exponent, which is still correct,
    // and anything at or past 0x7C00 is +/-inf.
    if (dropped > halfway || (dropped == halfway && (half & 1U) != 0)) ++half;
    return static_cast<std::uint16_t>(sign | std::min(half, 0x7C00U));
}

float float_from_half(const std::uint16_t half) noexcept {
    const std::uint32_t sign = std::uint32_t{half & 0x8000U} << 16;
    const std::uint32_t exponent = (half >> 10) & 0x1FU, mantissa = half & 0x03FFU;
    if (exponent != 0 && exponent != 0x1F) return std::bit_cast<float>(sign | ((exponent + 112) << 23) | (mantissa << 13));
    if (exponent == 0) {
        const float subnormal = static_cast<float>(mantissa) * 0x1p-24F;  // exact: at most 10 bits, no underflow
        return sign != 0 ? -subnormal : subnormal;
    }
    // inf, or NaN with its payload kept and the quiet bit set (as VCVTPH2PS does)
    return std::bit_cast<float>(sign | 0x7F80'0000U | (mantissa << 13) | (mantissa != 0 ? 0x0040'0000U : 0U));
}

EmbeddingMatrix::EmbeddingMatrix(const std::size_t dim) : dim_{dim} {
    if (dim == 0) throw std::invalid_argument("embedding dimension must be positive");
}

void EmbeddingMatrix::append(const std::span<const float> vector) {
    if (vector.size() != dim_) throw std::invalid_argument("embedding has the wrong dimension");
    // Double cannot overflow or underflow here: a float squared stays within 1e-90..1e77.
    double squares = 0.0;
    for (const float value : vector) {
        if (!std::isfinite(value)) throw std::invalid_argument("embedding is not finite");
        squares += double{value} * value;
    }
    if (squares == 0.0) throw std::invalid_argument("embedding is the zero vector");
    const double scale = 1.0 / std::sqrt(squares);
    const std::size_t start = halves_.size();
    halves_.resize(start + dim_);
    for (std::size_t i = 0; i < dim_; ++i)
        halves_[start + i] = half_from_float(static_cast<float>(vector[i] * scale));
}

std::size_t EmbeddingMatrix::rows() const noexcept { return halves_.size() / dim_; }

std::size_t EmbeddingMatrix::dim() const noexcept { return dim_; }

float EmbeddingMatrix::dot(const std::size_t row, const std::span<const float> query) const noexcept {
    if (row >= rows() || query.size() != dim_) return 0.0F;
    const std::uint16_t* halves = halves_.data() + row * dim_;
    float sum = 0.0F;
    for (std::size_t i = 0; i < dim_; ++i) sum += float_from_half(halves[i]) * query[i];
    return sum;
}

float EmbeddingMatrix::cosine(const std::size_t a, const std::size_t b) const noexcept {
    if (a >= rows() || b >= rows()) return 0.0F;
    const std::uint16_t* x = halves_.data() + a * dim_;
    const std::uint16_t* y = halves_.data() + b * dim_;
    float xy = 0.0F, xx = 0.0F, yy = 0.0F;
    for (std::size_t i = 0; i < dim_; ++i) {
        const float u = float_from_half(x[i]), v = float_from_half(y[i]);
        xy += u * v;
        xx += u * u;
        yy += v * v;
    }
    return xx > 0.0F && yy > 0.0F ? xy / (std::sqrt(xx) * std::sqrt(yy)) : 0.0F;
}

void EmbeddingMatrix::copy_row(const std::size_t row, const std::span<float> out) const noexcept {
    if (row >= rows() || out.size() != dim_) {
        std::ranges::fill(out, 0.0F);
        return;
    }
    std::ranges::transform(std::span{halves_}.subspan(row * dim_, dim_), out.begin(), float_from_half);
}

std::span<const std::uint16_t> EmbeddingMatrix::raw() const noexcept { return halves_; }

EmbeddingMatrix EmbeddingMatrix::from_raw(const std::size_t dim, std::vector<std::uint16_t> halves) {
    EmbeddingMatrix matrix(dim);
    if (halves.size() % dim != 0) throw std::invalid_argument("embedding data is not a whole number of rows");
    if (std::ranges::any_of(halves, [](const std::uint16_t h) { return (h & 0x7C00U) == 0x7C00U; }))
        throw std::invalid_argument("embedding data holds inf or NaN");
    matrix.halves_ = std::move(halves);
    return matrix;
}

}  // namespace pms
