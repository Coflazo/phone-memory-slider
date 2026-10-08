#include <algorithm>
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

    auto webm = bytes({0x1A, 0x45, 0xDF, 0xA3, 0x9F, 0x42, 0x86, 0x81, 0x01, 0x42, 0x82, 0x84});
    append(webm, ascii("webm"));
    const auto w = pms::sniff_media(webm);
    pms_check(w.format == MediaFormat::WebM && w.video, "EBML DocType webm is WebM");
    auto mkv = bytes({0x1A, 0x45, 0xDF, 0xA3, 0x9F, 0x42, 0x82, 0x88});
    append(mkv, ascii("matroska"));
    pms_check(pms::sniff_media(mkv).format == MediaFormat::Matroska, "EBML DocType matroska is Matroska");
    auto avi = ascii("RIFF"); append(avi, bytes({0, 1, 0, 0})); append(avi, ascii("AVI LIST"));
    const auto v = pms::sniff_media(avi);
    pms_check(v.format == MediaFormat::Avi && v.video, "RIFF AVI is video");
    const auto mm_tiff = ascii("MM\0*\0\0\0\x08"sv);
    pms_check(pms::sniff_media(mm_tiff).format == MediaFormat::Tiff, "big-endian TIFF");
    const auto three_gp = ftyp("3gp4", {"isom", "3gp4"});
    const auto t = pms::sniff_media(three_gp);
    pms_check(t.format == MediaFormat::ThreeGp && t.video, "3GP that also lists isom stays 3GP");
    const auto avis = ftyp("avis", {"avif", "msf1", "miaf"});
    const auto s = pms::sniff_media(avis);
    pms_check(s.format == MediaFormat::Avif && s.animated && !s.video, "avis is an animated AVIF");
    const auto avif_mif1 = ftyp("mif1", {"avif", "mif1", "miaf"});
    const auto m = pms::sniff_media(avif_mif1);
    pms_check(m.format == MediaFormat::Avif && !m.animated, "AVIF with generic major brand mif1 stays AVIF");
    auto webp_still = ascii("RIFF"); append(webp_still, bytes({30, 0, 0, 0})); append(webp_still, ascii("WEBPVP8X"));
    append(webp_still, bytes({10, 0, 0, 0, 0x10, 0, 0, 0}));  // alpha flag only
    const auto ws = pms::sniff_media(webp_still);
    pms_check(ws.format == MediaFormat::WebP && !ws.animated, "WebP without the animation flag is still");

    // Audio-only and Canon RAW ISO files must not enter the video pipeline.
    const auto m4a = ftyp("M4A ", {"M4A ", "mp42", "isom"});
    for (const auto major : {"M4A "sv, "M4B "sv, "M4P "sv, "crx "sv}) {
        pms_check(pms::sniff_media(ftyp(major, {"isom", "mp42"})).format == MediaFormat::Unknown,
                  "audio and Canon RAW major brands are Unknown");
    }
    // hevc/hevx are HEVC image-sequence brands, like msf1/hevs/hevm.
    const auto hevc = ftyp("hevc", {"mif1", "hevc"});
    const auto h = pms::sniff_media(hevc);
    pms_check(h.format == MediaFormat::Heif && h.animated, "hevc major brand is an image sequence");
    pms_check(pms::sniff_media(ftyp("heix", {"mif1", "hevx"})).animated, "hevx compatible brand is an image sequence");

    // Only the first sniff_bytes are read: a DocType past them is never seen.
    auto long_mkv = bytes({0x1A, 0x45, 0xDF, 0xA3});
    long_mkv.resize(pms::sniff_bytes);
    append(long_mkv, bytes({0x42, 0x82, 0x84})); append(long_mkv, ascii("webm"));
    pms_check(pms::sniff_media(long_mkv).format == MediaFormat::Matroska, "input is clamped to sniff_bytes");

    std::vector<std::string_view> names;
    for (int f = 0; f <= static_cast<int>(MediaFormat::Avi); ++f) {
        names.push_back(pms::to_string(static_cast<MediaFormat>(f)));
    }
    std::ranges::sort(names);
    pms_check(std::ranges::adjacent_find(names) == names.end() && !names.front().empty(), "every format has its own name");
    pms_check(pms::to_string(MediaFormat::ThreeGp) == "3gp" && pms::to_string(static_cast<MediaFormat>(99)) == "unknown",
              "to_string names and out-of-range fallback");

    // Every prefix of every sample stays inside the span (Debug checks spans) and reads as Unknown or
    // the full sample's format. Two samples name their family before the distinguishing bytes, so a
    // prefix may also stop at the family: Matroska before the webm DocType, HEIF before the avif brand.
    struct Sweep { const std::vector<std::byte>* sample; MediaFormat family; };
    for (const auto [sample, family] : {Sweep{&apng, MediaFormat::Unknown}, Sweep{&gif, MediaFormat::Unknown},
                                         Sweep{&gif_frames, MediaFormat::Unknown}, Sweep{&webp, MediaFormat::Unknown},
                                         Sweep{&liar, MediaFormat::Unknown}, Sweep{&avi, MediaFormat::Unknown},
                                         Sweep{&mm_tiff, MediaFormat::Unknown}, Sweep{&three_gp, MediaFormat::Unknown},
                                         Sweep{&avis, MediaFormat::Unknown}, Sweep{&webp_still, MediaFormat::Unknown},
                                         Sweep{&m4a, MediaFormat::Unknown}, Sweep{&hevc, MediaFormat::Unknown},
                                         Sweep{&webm, MediaFormat::Matroska}, Sweep{&avif_mif1, MediaFormat::Heif}}) {
        const auto full = pms::sniff_media(*sample).format;
        for (std::size_t n = 0; n <= sample->size(); ++n) {
            const auto got = pms::sniff_media(std::span<const std::byte>{*sample}.first(n)).format;
            pms_check(got == MediaFormat::Unknown || got == full || got == family,
                      "a truncated sample never sniffs as a different format");
        }
    }
}
