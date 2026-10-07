#include <algorithm>
#include <cmath>
#include <cstdint>
#include <random>
#include <vector>
#include "pms/quality.hpp"
#include "test_harness.hpp"

namespace {
std::vector<std::uint8_t> checker(std::uint32_t w, std::uint32_t h, int cell) {
    std::vector<std::uint8_t> p(std::size_t{w} * h);
    for (std::uint32_t y = 0; y < h; ++y)
        for (std::uint32_t x = 0; x < w; ++x) p[y * w + x] = ((x / cell + y / cell) % 2) ? 220 : 30;
    return p;
}
std::vector<std::uint8_t> box_blur(const std::vector<std::uint8_t>& s, std::uint32_t w, std::uint32_t h, int r) {
    std::vector<std::uint8_t> d(s.size());
    for (std::uint32_t y = 0; y < h; ++y)
        for (std::uint32_t x = 0; x < w; ++x) {
            int sum = 0, n = 0;
            for (int dy = -r; dy <= r; ++dy)
                for (int dx = -r; dx <= r; ++dx) {
                    const int xx = int(x) + dx, yy = int(y) + dy;
                    if (xx >= 0 && yy >= 0 && xx < int(w) && yy < int(h)) { sum += s[yy * w + xx]; ++n; }
                }
            d[y * w + x] = static_cast<std::uint8_t>(sum / n);
        }
    return d;
}
bool same(const pms::Quality& a, const pms::Quality& b) {
    return a.sharpness == b.sharpness && a.clipped_fraction == b.clipped_fraction && a.luma_p1 == b.luma_p1 &&
           a.luma_p99 == b.luma_p99 && a.noise_sigma == b.noise_sigma;
}
}  // namespace

void run_quality_tests() {
    const std::uint32_t w = 256, h = 192;
    const auto sharp = checker(w, h, 8);
    const auto soft = box_blur(sharp, w, h, 4);
    const auto qs = pms::measure_quality({sharp.data(), w, h, w});
    const auto qb = pms::measure_quality({soft.data(), w, h, w});
    pms_check(qs.sharpness > 4.0F * qb.sharpness, "blur lowers sharpness a lot");

    std::vector<std::uint8_t> flat(std::size_t{w} * h, 128);
    pms_check(pms::measure_quality({flat.data(), w, h, w}).noise_sigma < 0.01F, "flat image has no noise");
    std::mt19937 rng(1);
    std::normal_distribution<float> n(0.0F, 5.0F);
    for (auto& v : flat) v = static_cast<std::uint8_t>(std::clamp(128.0F + n(rng), 0.0F, 255.0F));
    const float sigma = pms::measure_quality({flat.data(), w, h, w}).noise_sigma;
    pms_check(sigma > 4.0F && sigma < 6.0F, "Immerkaer estimate tracks sigma=5");

    std::vector<std::uint8_t> white(std::size_t{w} * h, 255);
    pms_check(pms::measure_quality({white.data(), w, h, w}).clipped_fraction > 0.99F, "blown-out image is clipped");

    // Bokeh: sharp subject in one tile, blur elsewhere, still reads sharp.
    auto bokeh = soft;
    for (std::uint32_t y = 0; y < h / 4; ++y)
        for (std::uint32_t x = 0; x < w / 4; ++x) bokeh[y * w + x] = sharp[y * w + x];
    pms_check(pms::measure_quality({bokeh.data(), w, h, w}).sharpness > 0.5F * qs.sharpness, "max-tile sharpness ignores blurred background");
    std::uint8_t tiny[4] = {};
    pms_check(pms::measure_quality({tiny, 2, 2, 2}).sharpness == 0.0F, "tiny images return zeros");

    // Horizontal ramp 0..255: each value is 1/256 of the pixels.
    std::vector<std::uint8_t> ramp(std::size_t{256} * 8);
    for (std::size_t i = 0; i < ramp.size(); ++i) ramp[i] = static_cast<std::uint8_t>(i % 256);
    const auto qr = pms::measure_quality({ramp.data(), 256, 8, 256});
    pms_check(qr.luma_p1 == 2.0F && qr.luma_p99 == 253.0F, "percentiles are the first value reaching 1% and 99%");
    pms_check(std::abs(qr.clipped_fraction - 6.0F / 256.0F) < 1e-6F, "clipped counts 0..2 and 253..255");

    // Padded rows: the padding is never read, so results match the tight buffer exactly.
    const std::uint32_t stride = w + 13;
    std::vector<std::uint8_t> padded(std::size_t{stride} * h, 255);
    for (std::uint32_t y = 0; y < h; ++y) std::copy_n(bokeh.begin() + y * w, w, padded.begin() + y * stride);
    pms_check(same(pms::measure_quality({padded.data(), w, h, stride}), pms::measure_quality({bokeh.data(), w, h, w})),
              "stride padding is ignored");
    pms_check(same(pms::measure_quality({bokeh.data(), w, h, w - 1}), pms::Quality{}), "stride shorter than a row returns zeros");
}
