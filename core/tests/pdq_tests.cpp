#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <vector>
#include "pms/pdq.hpp"
#include "test_harness.hpp"

namespace {
std::vector<std::uint8_t> scene(std::uint32_t w, std::uint32_t h, int shift, int gain) {
    std::vector<std::uint8_t> p(std::size_t{w} * h * 3);
    for (std::uint32_t y = 0; y < h; ++y)
        for (std::uint32_t x = 0; x < w; ++x) {
            const int v = ((x + shift) * 7 + y * 3) % 200 + ((x / 32 + y / 32) % 2) * 40 + gain;
            for (int c = 0; c < 3; ++c) p[(y * w + x) * 3 + c] = static_cast<std::uint8_t>(std::clamp(v + c * 10, 0, 255));
        }
    return p;
}

bool is_empty(const pms::PdqHash& h) {
    return h.quality == 0 && std::ranges::all_of(h.bits, [](const std::uint64_t word) { return word == 0; });
}
}  // namespace

void run_pdq_tests() {
    const auto a = scene(320, 240, 0, 0), b = scene(320, 240, 0, 12), c = scene(320, 240, 97, 0);
    const auto ha = pms::pdq_hash({a.data(), 320, 240, 320 * 3});
    const auto hb = pms::pdq_hash({b.data(), 320, 240, 320 * 3});
    const auto hc = pms::pdq_hash({c.data(), 320, 240, 320 * 3});
    pms_check(pms::hamming(ha, ha) == 0, "identical images hash identically");
    pms_check(pms::hamming(ha, hb) <= 31, "brightness change stays a near duplicate");
    pms_check(pms::hamming(ha, hc) > 31, "a different scene is not a near duplicate");
    std::vector<std::uint8_t> flat(320 * 240 * 3, 90);
    pms_check(pms::pdq_hash({flat.data(), 320, 240, 960}).quality < 50, "flat images report low PDQ quality");

    // PDQ sets bits above the DCT median, so a real image has about half of its 256 bits set.
    int set_bits = 0;
    for (const std::uint64_t word : ha.bits) set_bits += std::popcount(word);
    pms_check(set_bits > 96 && set_bits < 160, "a textured image sets about half the bits");
    pms_check(ha.quality >= 50, "a textured image reports usable PDQ quality");

    // Channel order: red carries scene a and blue scene c, so luma 0.299R + 0.114B must follow scene a.
    std::vector<std::uint8_t> red_blue(a.size(), 0);
    for (std::size_t i = 0; i < red_blue.size(); i += 3) {
        red_blue[i] = a[i];
        red_blue[i + 2] = c[i];
    }
    const auto hrb = pms::pdq_hash({red_blue.data(), 320, 240, 960});
    pms_check(pms::hamming(hrb, ha) < pms::hamming(hrb, hc), "red is weighted above blue in the luma");

    // Row padding is skipped: the same pixels behind a longer stride hash identically.
    constexpr std::uint32_t padded_stride = 320 * 3 + 7;
    std::vector<std::uint8_t> padded(std::size_t{padded_stride} * 240, 255);
    for (std::size_t y = 0; y < 240; ++y) std::copy_n(a.begin() + static_cast<std::ptrdiff_t>(y * 960), 960, padded.begin() + static_cast<std::ptrdiff_t>(y * padded_stride));
    const auto hp = pms::pdq_hash({padded.data(), 320, 240, padded_stride});
    pms_check(hp.bits == ha.bits && hp.quality == ha.quality, "stride padding does not change the hash");

    // Invalid input and images below PDQ's 5x5 minimum give an empty hash with quality 0.
    pms_check(is_empty(pms::pdq_hash({nullptr, 320, 240, 960})), "null pixels give an empty hash");
    pms_check(is_empty(pms::pdq_hash({a.data(), 320, 240, 959})), "a stride shorter than width*3 gives an empty hash");
    pms_check(is_empty(pms::pdq_hash({a.data(), 0, 240, 960})), "zero width gives an empty hash");
    pms_check(is_empty(pms::pdq_hash({a.data(), 320, 0, 960})), "zero height gives an empty hash");
    pms_check(is_empty(pms::pdq_hash({a.data(), 4, 240, 960})), "width below 5 gives an empty hash");
    pms_check(is_empty(pms::pdq_hash({a.data(), 320, 4, 960})), "height below 5 gives an empty hash");
    pms_check(!is_empty(pms::pdq_hash({a.data(), 5, 5, 960})), "5x5 is the smallest hashable image");

    // A side above 4096 px means the caller skipped the resize: refused rather than blurred at full size.
    const auto wide = scene(4097, 5, 0, 0), tall = scene(5, 4097, 0, 0);
    pms_check(is_empty(pms::pdq_hash({wide.data(), 4097, 5, 4097 * 3})), "a width above 4096 gives an empty hash");
    pms_check(is_empty(pms::pdq_hash({tall.data(), 5, 4097, 5 * 3})), "a height above 4096 gives an empty hash");
    pms_check(!is_empty(pms::pdq_hash({wide.data(), 4096, 5, 4097 * 3})), "a width of 4096 is still hashed");
    pms_check(!is_empty(pms::pdq_hash({tall.data(), 5, 4096, 5 * 3})), "a height of 4096 is still hashed");

    // Regression pin. Provenance: computed by the vendored PDQ (ThreatExchange 85978d7) through this wrapper,
    // MSVC 19.43 x64 Release and Debug; PDQ hex 7dffabea54004155 5155015541550155 4155feaaaeaa7eaa aeaafe88aeaafe88
    // (bits[3]..bits[0]). The same wrapper reproduces upstream's reg-test hash for
    // pdq/data/reg-test-input/dih/bridge-1-original.jpg bit for bit (checked offline; core has no JPEG decoder).
    // This image's closest DCT coefficient sits 4.4e-4 (relative) from the median and no bit moved under 1e-5
    // relative luma noise, so float rounding or FMA differences between toolchains should not flip a bit.
    const auto pin = scene(320, 240, 13, 0);
    const auto hpin = pms::pdq_hash({pin.data(), 320, 240, 320 * 3});
    const std::array<std::uint64_t, 4> pinned{0xaeaafe88aeaafe88ULL, 0x4155feaaaeaa7eaaULL, 0x5155015541550155ULL, 0x7dffabea54004155ULL};
    pms_check(hpin.bits == pinned && hpin.quality == 100, "the reference image keeps its pinned PDQ hash");

    pms::PdqHash x{}, y{};
    x.bits = {0xFFFF'FFFF'FFFF'FFFFULL, 0, 1, 0x8000'0000'0000'0000ULL};
    pms_check(pms::hamming(x, y) == 66 && pms::hamming(y, x) == 66, "hamming counts differing bits in all four words");
}
