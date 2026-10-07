#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include "pms/media_format.hpp"
#include "test_harness.hpp"

namespace {
std::vector<std::byte> bytes(std::initializer_list<int> v) {
    std::vector<std::byte> out;
    for (int x : v) out.push_back(static_cast<std::byte>(x));
    return out;
}
std::vector<std::byte> ascii(std::string_view s) {
    std::vector<std::byte> out;
    for (char c : s) out.push_back(static_cast<std::byte>(c));
    return out;
}
void append(std::vector<std::byte>& a, const std::vector<std::byte>& b) { a.insert(a.end(), b.begin(), b.end()); }
std::vector<std::byte> be32(std::uint32_t v) { return bytes({int(v >> 24), int((v >> 16) & 255), int((v >> 8) & 255), int(v & 255)}); }
std::vector<std::byte> ftyp(std::string_view major, std::initializer_list<std::string_view> compat) {
    std::vector<std::byte> box = ascii("ftyp");
    append(box, ascii(major));
    append(box, bytes({0, 0, 0, 0}));
    for (auto c : compat) append(box, ascii(c));
    std::vector<std::byte> out = be32(static_cast<std::uint32_t>(box.size() + 4));
    append(out, box);
    return out;
}
}  // namespace

void run_media_format_tests() {
    using pms::MediaFormat;
    using namespace std::string_view_literals;
    pms_check(pms::sniff_media({}).format == MediaFormat::Unknown, "empty input is Unknown");
    pms_check(pms::sniff_media(bytes({0xFF, 0xD8})).format == MediaFormat::Unknown, "truncated JPEG magic is Unknown");
    pms_check(pms::sniff_media(bytes({0xFF, 0xD8, 0xFF, 0xE1})).format == MediaFormat::Jpeg, "JPEG magic");

    auto png = bytes({0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A});
    auto still = png;
    append(still, be32(0)); append(still, ascii("IDAT")); append(still, be32(0));
    pms_check(!pms::sniff_media(still).animated, "PNG without acTL is still");
    auto apng = png;
    append(apng, be32(8)); append(apng, ascii("acTL")); append(apng, bytes({0,0,0,2,0,0,0,0})); append(apng, be32(0));
    append(apng, be32(0)); append(apng, ascii("IDAT")); append(apng, be32(0));
    const auto a = pms::sniff_media(apng);
    pms_check(a.format == MediaFormat::Png && a.animated, "APNG is an animated PNG");

    auto gif = ascii("GIF89a");
    append(gif, bytes({1, 0, 1, 0, 0, 0, 0}));
    append(gif, ascii("!\xFF\x0BNETSCAPE2.0"));
    pms_check(pms::sniff_media(gif).animated, "GIF with NETSCAPE loop block is animated");

    // No NETSCAPE block: animation is only visible as a second image descriptor.
    auto gif_head = ascii("GIF89a");
    append(gif_head, bytes({1, 0, 1, 0, 0x80, 0, 0, 0, 0, 0, 255, 255, 255}));  // 2-entry global color table
    const auto frame = bytes({0x21, 0xF9, 0x04, 0x04, 0x0A, 0x00, 0x00, 0x00,     // graphic control extension
                              0x2C, 0, 0, 0, 0, 1, 0, 1, 0, 0, 0x02, 0x02, 0x4C, 0x01, 0x00});
    auto gif_still = gif_head;
    append(gif_still, frame); append(gif_still, bytes({0x3B}));
    const auto one_frame = pms::sniff_media(gif_still);
    pms_check(one_frame.format == MediaFormat::Gif && !one_frame.animated, "single-frame GIF is still");
    auto gif_frames = gif_head;
    append(gif_frames, frame); append(gif_frames, frame); append(gif_frames, bytes({0x3B}));
    pms_check(pms::sniff_media(gif_frames).animated, "GIF with two image descriptors is animated");

    auto webp = ascii("RIFF"); append(webp, bytes({30, 0, 0, 0})); append(webp, ascii("WEBPVP8X"));
    append(webp, bytes({10, 0, 0, 0, 0x02, 0, 0, 0}));
    pms_check(pms::sniff_media(webp).animated, "WebP VP8X animation flag");

    pms_check(pms::sniff_media(ftyp("heic", {"mif1", "heic"})).format == MediaFormat::Heif, "HEIC brand");
    pms_check(pms::sniff_media(ftyp("msf1", {"msf1", "hevc"})).animated, "HEIF image sequence is animated");
    pms_check(pms::sniff_media(ftyp("avif", {"avif", "mif1"})).format == MediaFormat::Avif, "AVIF brand");
    const auto mov = pms::sniff_media(ftyp("qt  ", {"qt  "}));
    pms_check(mov.format == MediaFormat::QuickTime && mov.video, "QuickTime is video");
    const auto mp4 = pms::sniff_media(ftyp("isom", {"isom", "avc1"}));
    pms_check(mp4.format == MediaFormat::Mp4 && mp4.video, "MP4 is video");

    // A huge box length must not be trusted.
    auto liar = ftyp("heic", {"mif1"});
    liar[0] = std::byte{0x7F};
    pms_check(pms::sniff_media(liar).format == MediaFormat::Heif, "ftyp with oversized length still sniffs safely");

    pms_check(pms::sniff_media(bytes({0x1A, 0x45, 0xDF, 0xA3})).video, "EBML is video");
    pms_check(pms::sniff_media(ascii("II*\0"sv)).format == MediaFormat::Tiff, "little-endian TIFF/DNG");

    // Every truncation point of every parsed vector must stay inside the span (checked in Debug builds).
    for (const auto* sample : {&apng, &gif, &gif_frames, &webp, &liar}) {
        for (std::size_t n = 0; n <= sample->size(); ++n) {
            (void)pms::sniff_media(std::span<const std::byte>{*sample}.first(n));
        }
    }
}
