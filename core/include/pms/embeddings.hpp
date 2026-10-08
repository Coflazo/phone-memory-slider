#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace pms {

// IEEE 754 binary16, bit-identical to x86 F16C (VCVTPS2PH with round to nearest even, VCVTPH2PS) on every input.
// half_from_float rounds to nearest, ties to even, keeps subnormals and sends magnitudes from 65520 up to +/-inf.
// Both directions keep a NaN's sign and payload (its top 10 bits when narrowing) and set the quiet bit, so a
// signalling NaN half comes back quieted (h | 0x0200); every other half round-trips bit for bit.
[[nodiscard]] std::uint16_t half_from_float(float value) noexcept;
[[nodiscard]] float float_from_half(std::uint16_t half) noexcept;  // exact: every half is a float

// Row-major unit vectors stored as fp16 (250,000 SigLIP2 rows x 768 = 384 MB); all arithmetic is fp32.
// Move-only, so 384 MB is never copied by accident; moving hands over the buffer. A deliberate copy is
// from_raw(m.dim(), {m.raw().begin(), m.raw().end()}).
class EmbeddingMatrix {
public:
    explicit EmbeddingMatrix(std::size_t dim = 768);  // throws std::invalid_argument for dim 0
    EmbeddingMatrix(const EmbeddingMatrix&) = delete;
    EmbeddingMatrix& operator=(const EmbeddingMatrix&) = delete;
    EmbeddingMatrix(EmbeddingMatrix&&) noexcept = default;
    EmbeddingMatrix& operator=(EmbeddingMatrix&&) noexcept = default;

    // L2-normalizes in double, then stores fp16. Throws std::invalid_argument, leaving the matrix unchanged,
    // for a size other than dim(), a non-finite value or the zero vector.
    void append(std::span<const float> vector);
    [[nodiscard]] std::size_t rows() const noexcept;
    [[nodiscard]] std::size_t dim() const noexcept;

    // An out-of-range row or a span whose size is not dim() gives 0 (dot, cosine) or zeros in `out` (copy_row).
    // dot is the plain dot product with the stored row, summed in float.
    [[nodiscard]] float dot(std::size_t row, std::span<const float> query) const noexcept;
    // a.b / (|a| |b|) over the stored halves, so cosine(a, a) is 1 despite fp16 rounding of the norm.
    [[nodiscard]] float cosine(std::size_t a, std::size_t b) const noexcept;
    void copy_row(std::size_t row, std::span<float> out) const noexcept;

    // The stored halves, rows() * dim() of them; from_raw takes them back bit for bit. from_raw throws
    // std::invalid_argument for dim 0, a size that is not a multiple of dim, or an inf or NaN half.
    [[nodiscard]] std::span<const std::uint16_t> raw() const noexcept;
    [[nodiscard]] static EmbeddingMatrix from_raw(std::size_t dim, std::vector<std::uint16_t> halves);

private:
    std::size_t dim_;
    std::vector<std::uint16_t> halves_;
};

}  // namespace pms
