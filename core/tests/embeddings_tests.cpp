#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>
#include "pms/embeddings.hpp"
#include "test_harness.hpp"

namespace {
bool is_nan_half(const std::uint32_t h) { return (h & 0x7C00U) == 0x7C00U && (h & 0x03FFU) != 0; }

// Value of a finite half from its fields, computed without float_from_half.
double half_value(const std::uint32_t h) {
    const int exponent = static_cast<int>((h >> 10) & 0x1FU), mantissa = static_cast<int>(h & 0x3FFU);
    const double magnitude = exponent == 0 ? std::ldexp(mantissa, -24) : std::ldexp(1024 + mantissa, exponent - 25);
    return (h & 0x8000U) != 0 ? -magnitude : magnitude;
}

std::uint16_t half(const float value) { return pms::half_from_float(value); }

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

void run_embeddings_tests() {
    for (float v : {0.0F, 1.0F, -2.5F, 65504.0F, 6.1035156e-05F, 0.333333F})
        pms_check(std::abs(pms::float_from_half(pms::half_from_float(v)) - v) <= std::abs(v) * 1e-3F + 1e-7F, "fp16 round trip");
    pms_check(std::isinf(pms::float_from_half(pms::half_from_float(1e9F))), "overflow becomes inf");
    pms::EmbeddingMatrix m(4);
    m.append(std::vector<float>{3, 0, 4, 0});
    m.append(std::vector<float>{0, 1, 0, 0});
    pms_check(m.rows() == 2, "two rows");
    pms_check(std::abs(m.dot(0, std::vector<float>{0.6F, 0, 0.8F, 0}) - 1.0F) < 1e-3F, "rows are unit length");
    pms_check(std::abs(m.cosine(0, 1)) < 1e-3F, "orthogonal rows");
    bool threw = false;
    try { m.append(std::vector<float>{1, 2}); } catch (const std::invalid_argument&) { threw = true; }
    pms_check(threw, "dimension mismatch throws");
    const auto copy = pms::EmbeddingMatrix::from_raw(4, {m.raw().begin(), m.raw().end()});
    pms_check(copy.rows() == 2 && std::abs(copy.cosine(0, 0) - 1.0F) < 1e-3F, "raw round trip");

    // Ties round to even. 1 + 2^-11 sits halfway between 1 (0x3C00, even) and 1 + 2^-10 (0x3C01).
    pms_check(half(1.0F + 0x1p-11F) == 0x3C00, "a tie above an even half rounds down");
    pms_check(half(1.0F + 3 * 0x1p-11F) == 0x3C02, "a tie above an odd half rounds up");
    pms_check(half(std::nextafter(1.0F + 0x1p-11F, 2.0F)) == 0x3C01, "just above a tie rounds up");
    pms_check(half(std::nextafter(1.0F + 3 * 0x1p-11F, 0.0F)) == 0x3C01, "just below a tie rounds down");
    pms_check(half(-(1.0F + 0x1p-11F)) == 0xBC00, "negative ties round to even too");
    pms_check(half(0x1p-25F) == 0x0000, "half the smallest subnormal ties to zero");
    pms_check(half(std::nextafter(0x1p-25F, 1.0F)) == 0x0001, "just above it is the smallest subnormal");
    pms_check(half(3 * 0x1p-25F) == 0x0002 && half(5 * 0x1p-25F) == 0x0002, "subnormal ties round to even");
    pms_check(half(1023.5F * 0x1p-24F) == 0x0400, "the tie between the largest subnormal and the smallest normal");
    pms_check(half(65520.0F) == 0x7C00, "the tie above 65504 overflows to inf");
    pms_check(half(std::nextafter(65520.0F, 0.0F)) == 0x7BFF, "just below that tie stays 65504");
    pms_check(half(std::numeric_limits<float>::max()) == 0x7C00 && half(-1e9F) == 0xFC00, "overflow keeps the sign");

    // Specials.
    constexpr float inf = std::numeric_limits<float>::infinity();
    pms_check(half(0.0F) == 0x0000 && half(-0.0F) == 0x8000, "signed zeros");
    pms_check(half(inf) == 0x7C00 && half(-inf) == 0xFC00, "infinities");
    pms_check(half(1e-40F) == 0x0000 && half(-1e-40F) == 0x8000, "float subnormals flush to signed zero");
    pms_check(half(std::numeric_limits<float>::quiet_NaN()) == 0x7E00, "the default NaN stays the default quiet NaN");
    pms_check(half(std::bit_cast<float>(0xFF80'0001U)) == 0xFE00, "a signalling NaN with low payload stays a NaN, quieted, sign kept");
    pms_check(half(std::bit_cast<float>(0x7FC0'2000U)) == 0x7E01, "NaN payload keeps its top bits");

    // Exhaustive: every one of the 65,536 halves widens exactly and narrows back to the same bits. NaNs come
    // back with the quiet bit (0x0200) set, so only the 2 x 511 signalling NaN halves change.
    int changed = 0;
    for (std::uint32_t h = 0; h <= 0xFFFFU; ++h) {
        const auto bits = static_cast<std::uint16_t>(h);
        const float f = pms::float_from_half(bits);
        const std::uint16_t back = pms::half_from_float(f);
        if (is_nan_half(h)) {
            pms_check(std::bit_cast<std::uint32_t>(f) == (((h & 0x8000U) << 16) | 0x7FC0'0000U | ((h & 0x03FFU) << 13)),
                      "NaN halves widen to quiet NaNs with their sign and payload");
            pms_check(back == static_cast<std::uint16_t>(h | 0x0200U), "NaN halves come back quieted with their payload");
            changed += back != bits ? 1 : 0;
            continue;
        }
        pms_check(back == bits, "every non-NaN half survives float and back bit for bit");
        pms_check(std::signbit(f) == ((h & 0x8000U) != 0), "float_from_half keeps the sign");
        if ((h & 0x7C00U) == 0x7C00U)
            pms_check(std::isinf(f), "the two infinities widen to infinities");
        else
            pms_check(static_cast<double>(f) == half_value(h), "float_from_half is exact");
    }
    pms_check(changed == 1022, "only the 1,022 signalling NaN halves change");

    // Exhaustive ties: between every pair of adjacent finite halves (and from 65504 to 2^16, which is inf) the
    // exact midpoint goes to the even one and the floats on either side of it go to the nearer one.
    for (std::uint32_t lo = 0; lo < 0x7C00U; ++lo) {
        const auto lo_half = static_cast<std::uint16_t>(lo), hi_half = static_cast<std::uint16_t>(lo + 1);
        const float a = pms::float_from_half(lo_half), b = lo == 0x7BFFU ? 65536.0F : pms::float_from_half(hi_half);
        const float mid = (a + b) / 2;  // exact: two halves have 11 significant bits, their mean at most 12
        const std::uint16_t even = lo % 2 == 0 ? lo_half : hi_half;
        pms_check(half(mid) == even && half(-mid) == (even | 0x8000), "every tie rounds to the even half");
        pms_check(half(std::nextafter(mid, 0.0F)) == lo_half, "every float below a tie rounds down");
        pms_check(half(std::nextafter(mid, inf)) == hi_half, "every float above a tie rounds up");
    }

    // append normalizes in double: 1e30^2 would overflow a float sum of squares and 1e-30^2 underflow it.
    pms::EmbeddingMatrix big(3);
    big.append(std::vector<float>{3e30F, 0, -4e30F});
    big.append(std::vector<float>{0, 1e-30F, 0});
    pms_check(big.raw()[0] == half(0.6F) && big.raw()[1] == 0 && big.raw()[2] == half(-0.8F), "huge vectors normalize");
    pms_check(big.raw()[4] == 0x3C00, "tiny vectors normalize");

    // A failed append leaves the matrix untouched.
    pms::EmbeddingMatrix guarded(3);
    guarded.append(std::vector<float>{1, 2, 3});
    const std::vector<std::uint16_t> before{guarded.raw().begin(), guarded.raw().end()};
    pms_check(throws_invalid_argument([&] { guarded.append(std::vector<float>{0, 0, 0}); }), "the zero vector throws");
    pms_check(throws_invalid_argument([&] { guarded.append(std::vector<float>{1, std::numeric_limits<float>::quiet_NaN(), 0}); }), "NaN throws");
    pms_check(throws_invalid_argument([&] { guarded.append(std::vector<float>{1, 0, -inf}); }), "inf throws");
    pms_check(throws_invalid_argument([&] { guarded.append(std::vector<float>{1, 2, 3, 4}); }), "too long throws");
    pms_check(guarded.rows() == 1 && std::vector<std::uint16_t>(guarded.raw().begin(), guarded.raw().end()) == before,
              "a rejected vector changes nothing");

    // dot, cosine and copy_row agree with double math on the stored rows.
    constexpr std::size_t dim = 768;
    pms::EmbeddingMatrix e(dim);
    std::vector<float> u(dim), v(dim), w(dim);
    for (std::size_t i = 0; i < dim; ++i) {
        u[i] = std::sin(0.37F * static_cast<float>(i) + 0.1F);
        v[i] = std::cos(0.11F * static_cast<float>(i)) + 0.5F * u[i];
        w[i] = static_cast<float>(i % 7) - 3.0F;
    }
    e.append(u);
    e.append(v);
    pms_check(e.rows() == 2 && e.dim() == dim && e.raw().size() == 2 * dim, "shape");
    std::vector<float> row(dim);
    e.copy_row(1, row);
    double norm = 0, uu = 0, vv = 0, uv = 0, row_dot_w = 0;
    for (std::size_t i = 0; i < dim; ++i) {
        pms_check(row[i] == pms::float_from_half(e.raw()[dim + i]), "copy_row widens the stored halves");
        norm += double{row[i]} * row[i];
        uu += double{u[i]} * u[i];
        vv += double{v[i]} * v[i];
        uv += double{u[i]} * v[i];
        row_dot_w += double{row[i]} * w[i];
    }
    pms_check(std::abs(std::sqrt(norm) - 1.0) < 1e-3, "stored rows are unit length within fp16 precision");
    pms_check(std::abs(e.dot(1, w) - row_dot_w) < 1e-4 * std::abs(row_dot_w) + 1e-4, "dot matches double math on the stored row");
    pms_check(std::abs(e.cosine(0, 1) - uv / std::sqrt(uu * vv)) < 1e-3, "cosine matches the cosine of the inputs");
    pms_check(std::abs(e.cosine(1, 1) - 1.0F) < 1e-6F && e.cosine(0, 1) == e.cosine(1, 0), "cosine is normalized and symmetric");

    // Out-of-range rows and wrong-size spans give 0 instead of reading past the data.
    pms_check(e.dot(2, w) == 0.0F && e.dot(0, std::vector<float>(dim - 1, 1.0F)) == 0.0F, "bad dot arguments give 0");
    pms_check(e.cosine(0, 2) == 0.0F && e.cosine(5, 0) == 0.0F, "bad cosine rows give 0");
    std::vector<float> filled(dim, 7.0F);
    e.copy_row(2, filled);
    pms_check(filled == std::vector<float>(dim, 0.0F), "copy_row of a missing row writes zeros");
    std::vector<float> short_out(3, 7.0F);
    e.copy_row(0, short_out);
    pms_check(short_out == std::vector<float>(3, 0.0F), "copy_row into the wrong size writes zeros");

    // from_raw takes the stored bits back exactly and validates them.
    const auto reloaded = pms::EmbeddingMatrix::from_raw(dim, {e.raw().begin(), e.raw().end()});
    pms_check(reloaded.rows() == 2 && reloaded.dot(1, w) == e.dot(1, w), "from_raw restores the same rows");
    pms_check(pms::EmbeddingMatrix::from_raw(4, {}).rows() == 0, "an empty matrix loads");
    pms_check(throws_invalid_argument([] { (void)pms::EmbeddingMatrix::from_raw(4, std::vector<std::uint16_t>(6, 0x3C00)); }),
              "a size that is not a multiple of dim throws");
    pms_check(throws_invalid_argument([] { (void)pms::EmbeddingMatrix::from_raw(2, {0x3C00, 0x7C00}); }), "stored inf throws");
    pms_check(throws_invalid_argument([] { (void)pms::EmbeddingMatrix::from_raw(2, {0x3C00, 0xFE00}); }), "stored NaN throws");
    pms_check(throws_invalid_argument([] { (void)pms::EmbeddingMatrix::from_raw(0, {}); }), "dim 0 throws in from_raw");
    pms_check(throws_invalid_argument([] { (void)pms::EmbeddingMatrix(0); }), "dim 0 throws");

    // Moving hands over the buffer without copying 384 MB, and copying does not compile.
    static_assert(std::is_nothrow_move_constructible_v<pms::EmbeddingMatrix> && std::is_nothrow_move_assignable_v<pms::EmbeddingMatrix>);
    static_assert(!std::is_copy_constructible_v<pms::EmbeddingMatrix> && !std::is_copy_assignable_v<pms::EmbeddingMatrix>);
    const std::uint16_t* data = e.raw().data();
    pms::EmbeddingMatrix moved = std::move(e);
    pms_check(moved.raw().data() == data && moved.rows() == 2, "moving keeps the same buffer");
}
