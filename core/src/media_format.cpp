#include "pms/media_format.hpp"

#include <algorithm>

namespace pms {
namespace {

using Bytes = std::span<const std::byte>;

constexpr auto same_byte = [](const std::byte b, const char c) noexcept {
    return b == static_cast<std::byte>(c);
};

[[nodiscard]] bool matches(const Bytes head, const std::size_t offset, const std::string_view magic) noexcept {
    return offset <= head.size() && head.size() - offset >= magic.size() &&
           std::ranges::equal(head.subspan(offset, magic.size()), magic, same_byte);
}

[[nodiscard]] bool contains(const Bytes head, const std::string_view needle) noexcept {
    return !std::ranges::search(head, needle, same_byte).empty();
}

[[nodiscard]] std::uint32_t be32_at(const Bytes head, const std::size_t offset) noexcept {
    if (offset > head.size() || head.size() - offset < 4) {
        return 0;
    }
    std::uint32_t value = 0;
    for (const auto b : head.subspan(offset, 4)) {
        value = (value << 8) | std::to_integer<std::uint32_t>(b);
    }
    return value;
}

[[nodiscard]] consteval std::uint32_t fourcc(const char (&code)[5]) noexcept {
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        value = (value << 8) | static_cast<unsigned char>(code[i]);
    }
    return value;
}

[[nodiscard]] SniffResult sniff_png(const Bytes head) noexcept {
    // Chunks: 4-byte BE length, 4-byte type, data, 4-byte CRC. acTL must precede IDAT.
    SniffResult result{MediaFormat::Png};
    std::size_t offset = 8;
    while (head.size() - offset >= 8) {
        if (matches(head, offset + 4, "acTL")) {
            result.animated = true;
            break;
        }
        if (matches(head, offset + 4, "IDAT")) {
            break;
        }
        const auto length = be32_at(head, offset);
        const auto remaining = head.size() - offset;
        if (remaining < 12 || length > remaining - 12) {
            break;
        }
        offset += 12 + std::size_t{length};
    }
    return result;
}

[[nodiscard]] std::size_t gif_color_table_size(const std::byte packed) noexcept {
    const auto flags = std::to_integer<unsigned>(packed);
    return (flags & 0x80U) != 0 ? std::size_t{3} << ((flags & 0x07U) + 1) : 0;
}

// Skips a chain of GIF data sub-blocks; returns the offset after its 0 terminator,
// or a value >= head.size() when the chain runs past the bytes we hold.
[[nodiscard]] std::size_t skip_gif_sub_blocks(const Bytes head, std::size_t offset) noexcept {
    while (offset < head.size()) {
        const auto size = std::to_integer<std::size_t>(head[offset]);
        offset += 1 + size;
        if (size == 0) {
            break;
        }
    }
    return offset;
}

[[nodiscard]] SniffResult sniff_gif(const Bytes head) noexcept {
    SniffResult result{MediaFormat::Gif};
    if (contains(head, "NETSCAPE2.0")) {
        result.animated = true;
        return result;
    }
    constexpr std::size_t screen_descriptor_end = 13;
    if (head.size() < screen_descriptor_end) {
        return result;
    }
    std::size_t offset = screen_descriptor_end + gif_color_table_size(head[10]);
    int images = 0;
    while (offset < head.size()) {
        const auto block = head[offset];
        if (block == std::byte{0x21}) {  // extension: introducer, label, sub-blocks
            offset = skip_gif_sub_blocks(head, offset + 2);
        } else if (block == std::byte{0x2C}) {  // image descriptor
            if (++images == 2) {
                result.animated = true;
                break;
            }
            if (head.size() - offset < 10) {
                break;
            }
            // 10-byte descriptor, optional local color table, LZW minimum code size, sub-blocks.
            offset += 10 + gif_color_table_size(head[offset + 9]) + 1;
            offset = skip_gif_sub_blocks(head, offset);
        } else {
            break;  // trailer 0x3B or garbage
        }
    }
    return result;
}

[[nodiscard]] SniffResult sniff_iso_bmff(const Bytes head) noexcept {
    // The declared ftyp size is untrusted: clamp it to the bytes we hold (0 means "to end").
    const std::size_t declared = be32_at(head, 0);
    const auto box_end = declared == 0 ? head.size() : std::min(declared, head.size());

    bool heif = false;
    bool heif_sequence = false;
    bool avif = false;
    bool avif_sequence = false;
    bool quicktime = false;
    bool three_gp = false;
    bool mp4 = false;
    const auto classify = [&](const std::uint32_t brand) noexcept {
        switch (brand) {
            case fourcc("msf1"):
            case fourcc("hevs"):
            case fourcc("hevm"):
                heif_sequence = true;
                [[fallthrough]];
            case fourcc("heic"):
            case fourcc("heix"):
            case fourcc("hevc"):
            case fourcc("heim"):
            case fourcc("heis"):
            case fourcc("mif1"):
            case fourcc("hevx"):
                heif = true;
                break;
            case fourcc("avis"):
                avif_sequence = true;
                [[fallthrough]];
            case fourcc("avif"):
                avif = true;
                break;
            case fourcc("qt  "):
                quicktime = true;
                break;
            case fourcc("3gp4"):
            case fourcc("3gp5"):
            case fourcc("3gp6"):
            case fourcc("3g2a"):
                three_gp = true;
                break;
            case fourcc("isom"):
            case fourcc("iso2"):
            case fourcc("mp41"):
            case fourcc("mp42"):
            case fourcc("avc1"):
            case fourcc("dash"):
            case fourcc("M4V "):
                mp4 = true;
                break;
            default:
                break;
        }
    };
    if (box_end >= 12) {
        classify(be32_at(head, 8));  // major brand
    }
    for (std::size_t offset = 16; offset + 4 <= box_end; offset += 4) {
        classify(be32_at(head, offset));  // compatible brands
    }

    // Most specific family wins: AVIF also lists the generic HEIF brand mif1,
    // and 3GP files usually list isom.
    if (avif) {
        return {MediaFormat::Avif, avif_sequence, false};
    }
    if (heif) {
        return {MediaFormat::Heif, heif_sequence, false};
    }
    if (quicktime) {
        return {MediaFormat::QuickTime, false, true};
    }
    if (three_gp) {
        return {MediaFormat::ThreeGp, false, true};
    }
    if (mp4) {
        return {MediaFormat::Mp4, false, true};
    }
    return {};
}

}  // namespace

SniffResult sniff_media(const std::span<const std::byte> head) noexcept {
    using namespace std::string_view_literals;
    if (matches(head, 0, "\xFF\xD8\xFF")) {
        return {MediaFormat::Jpeg};
    }
    if (matches(head, 0, "\x89PNG\r\n\x1A\n")) {
        return sniff_png(head);
    }
    if (matches(head, 0, "GIF87a") || matches(head, 0, "GIF89a")) {
        return sniff_gif(head);
    }
    if (matches(head, 0, "RIFF") && matches(head, 8, "WEBP")) {
        // VP8X is the first chunk when present; flag bit 0x02 marks animation.
        const auto animated = matches(head, 12, "VP8X") && head.size() > 20 &&
                              (std::to_integer<unsigned>(head[20]) & 0x02U) != 0;
        return {MediaFormat::WebP, animated};
    }
    if (matches(head, 0, "RIFF") && matches(head, 8, "AVI ")) {
        return {MediaFormat::Avi, false, true};
    }
    if (matches(head, 4, "ftyp")) {
        return sniff_iso_bmff(head);
    }
    if (matches(head, 0, "\x1A\x45\xDF\xA3")) {
        // ponytail: matches the DocType element only with a 1-byte size (what libwebm
        // and ffmpeg write); decode the EBML vint if a WebM ever sniffs as Matroska.
        return {contains(head, "\x42\x82\x84webm") ? MediaFormat::WebM : MediaFormat::Matroska, false, true};
    }
    if (matches(head, 0, "II*\0"sv) || matches(head, 0, "MM\0*"sv)) {
        return {MediaFormat::Tiff};
    }
    return {};
}

std::string_view to_string(const MediaFormat format) noexcept {
    switch (format) {
        case MediaFormat::Unknown: return "unknown";
        case MediaFormat::Jpeg: return "jpeg";
        case MediaFormat::Png: return "png";
        case MediaFormat::Gif: return "gif";
        case MediaFormat::WebP: return "webp";
        case MediaFormat::Heif: return "heif";
        case MediaFormat::Avif: return "avif";
        case MediaFormat::Tiff: return "tiff";
        case MediaFormat::Mp4: return "mp4";
        case MediaFormat::QuickTime: return "quicktime";
        case MediaFormat::ThreeGp: return "3gp";
        case MediaFormat::Matroska: return "matroska";
        case MediaFormat::WebM: return "webm";
        case MediaFormat::Avi: return "avi";
    }
    return "unknown";
}

}  // namespace pms
