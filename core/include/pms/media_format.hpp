#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace pms {

enum class MediaFormat : std::uint8_t {
    Unknown,
    Jpeg,
    Png,
    Gif,
    WebP,
    Heif,
    Avif,
    Tiff,
    Mp4,
    QuickTime,
    ThreeGp,
    Matroska,
    WebM,
    Avi,
};

struct SniffResult {
    MediaFormat format{MediaFormat::Unknown};
    bool animated{};
    bool video{};
};

inline constexpr std::size_t sniff_bytes = 65536;  // bytes past this many are ignored

// Types a file from its leading bytes, never its extension. `head` is untrusted:
// nothing past head.size() is read and no length field inside it is believed.
[[nodiscard]] SniffResult sniff_media(std::span<const std::byte> head) noexcept;

[[nodiscard]] std::string_view to_string(MediaFormat format) noexcept;

}  // namespace pms
