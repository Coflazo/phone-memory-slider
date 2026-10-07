#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include "pms/media_meta.hpp"
#include "test_harness.hpp"

namespace {
struct Tiff {  // tiny big-endian TIFF writer for tests
    std::vector<std::byte> b;
    void u8(int v) { b.push_back(static_cast<std::byte>(v)); }
    void u16(int v) { u8(v >> 8); u8(v & 255); }
    void u32(std::uint32_t v) { u16(int(v >> 16)); u16(int(v & 0xFFFF)); }
    void str(std::string_view s) { for (char c : s) u8(static_cast<unsigned char>(c)); }
    void put32(std::size_t at, std::uint32_t v) { for (int i = 0; i < 4; ++i) b[at + i] = static_cast<std::byte>((v >> (24 - 8 * i)) & 255); }
};

std::vector<std::byte> apple_exif(std::string_view content_id, std::string_view burst) {
    Tiff t;
    t.str("MM"); t.u16(42); t.u32(8);
    // IFD0: Make, ExifIFD pointer
    t.u16(2);
    t.u16(0x010F); t.u16(2); t.u32(6); const auto make_off = t.b.size(); t.u32(0);
    t.u16(0x8769); t.u16(4); t.u32(1); const auto exif_ptr = t.b.size(); t.u32(0);
    t.u32(0);
    t.put32(make_off, static_cast<std::uint32_t>(t.b.size())); t.str("Apple"); t.u8(0);
    t.put32(exif_ptr, static_cast<std::uint32_t>(t.b.size()));
    // ExifIFD: MakerNote (UNDEFINED)
    t.u16(1);
    const std::uint32_t mn_len = 14 + 2 + 2 * 12 + 4 + 64 + 64;
    t.u16(0x927C); t.u16(7); t.u32(mn_len); const auto mn_ptr = t.b.size(); t.u32(0);
    t.u32(0);
    const auto mn = t.b.size();
    t.put32(mn_ptr, static_cast<std::uint32_t>(mn));
    t.str("Apple iOS"); t.u8(0); t.u16(1); t.str("MM");
    t.u16(2);
    t.u16(0x000B); t.u16(2); t.u32(static_cast<std::uint32_t>(burst.size() + 1)); const auto burst_off = t.b.size(); t.u32(0);
    t.u16(0x0011); t.u16(2); t.u32(static_cast<std::uint32_t>(content_id.size() + 1)); const auto cid_off = t.b.size(); t.u32(0);
    t.u32(0);
    t.put32(burst_off, static_cast<std::uint32_t>(t.b.size() - mn)); t.str(burst); t.u8(0);
    t.put32(cid_off, static_cast<std::uint32_t>(t.b.size() - mn)); t.str(content_id); t.u8(0);
    return t.b;
}

// IFD0 Model + ExifIFD DateTimeOriginal, plus OffsetTimeOriginal when `offset` is not empty.
std::vector<std::byte> camera_exif(std::string_view taken, std::string_view offset, std::string_view model = "Pixel 8") {
    Tiff t;
    const auto here = [&] { return static_cast<std::uint32_t>(t.b.size()); };
    t.str("MM"); t.u16(42); t.u32(8);
    t.u16(2);
    t.u16(0x0110); t.u16(2); t.u32(static_cast<std::uint32_t>(model.size() + 1)); const auto model_off = t.b.size(); t.u32(0);
    t.u16(0x8769); t.u16(4); t.u32(1); const auto exif_ptr = t.b.size(); t.u32(0);
    t.u32(0);
    t.put32(model_off, here()); t.str(model); t.u8(0);
    t.put32(exif_ptr, here());
    t.u16(offset.empty() ? 1 : 2);
    t.u16(0x9003); t.u16(2); t.u32(20); const auto taken_off = t.b.size(); t.u32(0);
    std::size_t offset_off = 0;
    if (!offset.empty()) { t.u16(0x9011); t.u16(2); t.u32(7); offset_off = t.b.size(); t.u32(0); }
    t.u32(0);
    t.put32(taken_off, here()); t.str(taken); t.u8(0);
    if (!offset.empty()) { t.put32(offset_off, here()); t.str(offset); t.u8(0); }
    return t.b;
}

std::vector<std::byte> bytes(std::initializer_list<int> v) {
    std::vector<std::byte> out;
    for (int x : v) out.push_back(static_cast<std::byte>(x));
    return out;
}

// SOI, a small APP0, APP1 Exif, APP1 XMP, SOS.
std::vector<std::byte> jpeg_with(std::span<const std::byte> exif, std::string_view xmp) {
    using namespace std::string_view_literals;
    auto out = bytes({0xFF, 0xD8, 0xFF, 0xE0, 0, 4, 0, 0});
    const auto app1 = [&](std::string_view signature, std::span<const std::byte> body) {
        const auto len = 2 + signature.size() + body.size();
        out.insert(out.end(), {std::byte{0xFF}, std::byte{0xE1}, static_cast<std::byte>(len >> 8), static_cast<std::byte>(len & 255)});
        for (char c : signature) out.push_back(static_cast<std::byte>(c));
        out.insert(out.end(), body.begin(), body.end());
    };
    app1("Exif\0\0"sv, exif);
    app1("http://ns.adobe.com/xap/1.0/\0"sv, std::as_bytes(std::span{xmp}));
    const auto sos = bytes({0xFF, 0xDA, 0, 2});
    out.insert(out.end(), sos.begin(), sos.end());
    return out;
}
}  // namespace

void run_media_meta_tests() {
    const std::string xmp =
        R"(<x:xmpmeta><rdf:RDF><rdf:Description Camera:MotionPhoto="1" Camera:MotionPhotoVersion="1" )"
        R"(Camera:MotionPhotoPresentationTimestampUs="1234"><Container:Directory><rdf:Seq>)"
        R"(<rdf:li><Container:Item Item:Mime="image/jpeg" Item:Semantic="Primary" Item:Length="0"/></rdf:li>)"
        R"(<rdf:li><Container:Item Item:Mime="video/mp4" Item:Semantic="MotionPhoto" Item:Length="345678"/></rdf:li>)"
        R"(</rdf:Seq></Container:Directory></rdf:Description></rdf:RDF></x:xmpmeta>)";
    const auto mp = pms::parse_motion_photo_xmp(xmp);
    pms_check(mp && mp->video_length == 345678 && mp->presentation_us == 1234, "Motion Photo v1 directory");
    const auto legacy = pms::parse_motion_photo_xmp(R"(<x GCamera:MicroVideo="1" GCamera:MicroVideoOffset="99000"/>)");
    pms_check(legacy && legacy->video_length == 99000, "legacy MicroVideo offset");
    pms_check(!pms::parse_motion_photo_xmp(R"(<x Camera:MotionPhoto="0"/>)"), "MotionPhoto=0 is not a motion photo");
    pms_check(!pms::parse_motion_photo_xmp(R"(<x Item:Length="18446744073709551616" Camera:MotionPhoto="1"/>)"),
              "overflowing length is rejected");

    const auto exif = apple_exif("6F2A1C0E-AAAA-BBBB-CCCC-123456789ABC", "BURST-1");
    const auto facts = pms::parse_exif(exif);
    pms_check(facts.has_camera && facts.make == "Apple", "Make tag read from IFD0");
    pms_check(facts.content_identifier == "6F2A1C0E-AAAA-BBBB-CCCC-123456789ABC", "Apple ContentIdentifier");
    pms_check(facts.burst_uuid == "BURST-1", "Apple BurstUUID");

    auto broken = exif;
    broken.resize(broken.size() / 2);
    const auto partial = pms::parse_exif(broken);
    pms_check(partial.content_identifier.empty(), "truncated MakerNote yields no identifier, no crash");
    pms_check(!pms::parse_exif({}).has_camera, "empty EXIF");

    // The overflow case above has no MotionPhoto item, so pin the integer path itself.
    const auto item = [](std::string_view length) {
        return R"(<x Camera:MotionPhoto="1"><Container:Item Item:Semantic="MotionPhoto" Item:Length=")" +
               std::string{length} + R"("/></x>)";
    };
    const auto max = pms::parse_motion_photo_xmp(item("18446744073709551615"));
    pms_check(max && max->video_length == 18446744073709551615ULL && max->presentation_us == -1,
              "largest uint64 length parses, no timestamp means -1");
    pms_check(!pms::parse_motion_photo_xmp(item("18446744073709551616")), "item length overflow is rejected");
    pms_check(!pms::parse_motion_photo_xmp(item("-5")), "negative length is rejected");
    pms_check(!pms::parse_motion_photo_xmp(item("12abc")), "trailing garbage is rejected");
    pms_check(!pms::parse_motion_photo_xmp(item("0")), "zero-length video is not a motion photo");
    pms_check(!pms::parse_motion_photo_xmp(R"(<x GCamera:MicroVideo="1" GCamera:MicroVideoOffset="99999999999999999999"/>)"),
              "legacy offset overflow is rejected");

    // Little-endian TIFF with a value short enough to sit inline in the entry.
    const auto lg = pms::parse_exif(bytes({'I', 'I', 42, 0, 8, 0, 0, 0, 1, 0, 0x0F, 0x01, 2, 0, 3, 0, 0, 0, 'L', 'G', 0, 0, 0, 0, 0, 0}));
    pms_check(lg.has_camera && lg.make == "LG", "little-endian inline Make");

    const auto utc = pms::parse_exif(camera_exif("2024:05:06 07:08:09", ""));
    pms_check(utc.has_camera && utc.model == "Pixel 8" && utc.make.empty(), "Model alone counts as a camera");
    pms_check(utc.captured_unix_ms == 1714979289000, "DateTimeOriginal without offset is UTC");
    pms_check(pms::parse_exif(camera_exif("2024:05:06 07:08:09", "+02:00")).captured_unix_ms == 1714972089000,
              "OffsetTimeOriginal shifts local time to UTC");
    pms_check(pms::parse_exif(camera_exif("2024:02:30 07:08:09", "")).captured_unix_ms == 0, "impossible date is unknown");
    pms_check(pms::parse_exif(camera_exif("0000:00:00 00:00:00", "")).captured_unix_ms == 0, "zeroed date is unknown");

    auto flood = exif;
    flood[8] = std::byte{0x02};  // IFD0 entry count 0x0201 = 513
    flood[9] = std::byte{0x01};
    pms_check(!pms::parse_exif(flood).has_camera, "IFD with more than 512 entries is rejected");
    pms_check(pms::parse_exif(camera_exif("2024:05:06 07:08:09", "", std::string(300, 'A'))).model == std::string(256, 'A'),
              "ASCII values are capped at 256 bytes");

    const auto jpeg = jpeg_with(exif, xmp);
    const auto found_exif = pms::find_jpeg_exif(jpeg);
    pms_check(found_exif && std::ranges::equal(*found_exif, exif), "EXIF TIFF block found in APP1");
    const auto found_xmp = pms::find_jpeg_xmp(jpeg);
    pms_check(found_xmp && *found_xmp == xmp, "XMP packet found in a later APP1");
    pms_check(!pms::find_jpeg_xmp(exif) && !pms::find_jpeg_exif(exif), "non-JPEG has no segments");

    // Truncation sweeps. Debug builds also trap any out-of-range span access.
    for (std::size_t n = 0; n <= exif.size(); ++n) {
        const auto f = pms::parse_exif(std::span<const std::byte>{exif}.first(n));
        pms_check(f.content_identifier.empty() || f.content_identifier == facts.content_identifier,
                  "truncated EXIF never yields a partial ContentIdentifier");
        pms_check(f.burst_uuid.empty() || f.burst_uuid == facts.burst_uuid, "truncated EXIF never yields a partial BurstUUID");
        pms_check(f.make.empty() || f.make == "Apple", "truncated EXIF never yields a partial Make");
    }
    for (std::size_t n = 0; n <= xmp.size(); ++n) {
        const auto m = pms::parse_motion_photo_xmp(std::string_view{xmp}.substr(0, n));
        pms_check(!m || m->video_length == 345678, "truncated XMP never yields a wrong length");
    }
    for (std::size_t n = 0; n <= jpeg.size(); ++n) {
        const auto prefix = std::span<const std::byte>{jpeg}.first(n);
        const auto e = pms::find_jpeg_exif(prefix);
        pms_check(!e || (e->size() <= exif.size() && std::ranges::equal(*e, std::span<const std::byte>{exif}.first(e->size()))),
                  "EXIF from a JPEG prefix is a prefix of the block");
        const auto x = pms::find_jpeg_xmp(prefix);
        pms_check(!x || std::string_view{xmp}.starts_with(*x), "XMP from a JPEG prefix is a prefix of the packet");
    }
}
